#include "increment_controller.hpp"

#include "number_types.hpp"
#include "stdout_logger.hpp"

#include <drogon/drogon.h>

#include <atomic>
#include <charconv>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace linear_service {
namespace {

// Не допускаем отправку двух HTTP-ответов на один запрос, если асинхронные
// ветви завершения попытаются вызвать callback повторно.
class ResponseOnce {
public:
    explicit ResponseOnce(
        std::function<void(const drogon::HttpResponsePtr&)>&& callback)
        : callback_(std::move(callback)) {}

    void send(const drogon::HttpResponsePtr& response) {
        if (!sent_.exchange(true)) {
            callback_(response);
        }
    }

private:
    std::function<void(const drogon::HttpResponsePtr&)> callback_;
    std::atomic_bool sent_{false};
};

drogon::HttpResponsePtr jsonResponse(drogon::HttpStatusCode status,
                                     const Json::Value& body) {
    auto response = drogon::HttpResponse::newHttpJsonResponse(body);
    response->setStatusCode(status);
    return response;
}

drogon::HttpResponsePtr errorResponse(drogon::HttpStatusCode status,
                                      const std::string& code,
                                      const std::string& message,
                                      std::int64_t input = 0,
                                      bool include_input = false) {
    Json::Value body;
    body["error"]["code"] = code;
    body["error"]["message"] = message;
    if (include_input) {
        body["error"]["input"] = static_cast<Json::Int64>(input);
    }
    return jsonResponse(status, body);
}

// Принимаем только ASCII-цифры и проверяем переполнение до передачи числа
// сервису. string_view позволяет разобрать тело без дополнительной копии.
bool parseNumber(std::string_view body, std::int64_t& value) {
    if (body.empty() || body.size() > 64) {
        return false;
    }
    for (const char ch : body) {
        if (ch < '0' || ch > '9') {
            return false;
        }
    }
    const auto parsed = std::from_chars(body.data(), body.data() + body.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == body.data() + body.size();
}

// Преобразуем результат сценария в HTTP-код и JSON здесь, чтобы бизнес-сервис
// не зависел от HTTP и мог проверяться отдельно от веб-сервера.
void handleProcessResult(const ProcessResult& result,
                         const std::shared_ptr<ResponseOnce>& responder) {
    switch (result.status) {
        case ProcessStatus::Success: {
            Json::Value body;
            body["input"] = static_cast<Json::Int64>(result.input);
            body["result"] = static_cast<Json::Int64>(result.result);
            responder->send(jsonResponse(drogon::k200OK, body));
            return;
        }
        case ProcessStatus::DuplicateNumber:
            responder->send(errorResponse(
                drogon::k409Conflict, "DUPLICATE_NUMBER",
                "The number has already been received.", result.input, true));
            return;
        case ProcessStatus::PredecessorOfProcessedNumber: {
            Json::Value body;
            body["error"]["code"] = "PREDECESSOR_OF_PROCESSED_NUMBER";
            body["error"]["input"] = static_cast<Json::Int64>(result.input);
            body["error"]["already_processed"] =
                static_cast<Json::Int64>(result.related_number);
            body["error"]["message"] =
                "The number is one less than a number already processed.";
            responder->send(jsonResponse(drogon::k409Conflict, body));
            return;
        }
        case ProcessStatus::InvalidNumber:
            responder->send(errorResponse(
                drogon::k400BadRequest, "INVALID_NUMBER",
                "The number must be non-negative.", result.input, true));
            return;
        case ProcessStatus::NumberOutOfRange:
            responder->send(errorResponse(
                drogon::k400BadRequest, "NUMBER_OUT_OF_RANGE",
                "The number is above the configured maximum.", result.input, true));
            return;
        case ProcessStatus::DatabaseError:
            responder->send(errorResponse(
                drogon::k503ServiceUnavailable, "SERVICE_UNAVAILABLE",
                "The request could not be completed because the data service failed."));
            return;
    }
}

}  // namespace

// Связываем HTTP-маршруты с контрактом INumberService; конкретный сервис
// можно заменить, не переписывая обработку HTTP-запросов.
void registerIncrementRoutes(std::shared_ptr<INumberService> service) {
    if (!service) {
        throw std::invalid_argument("registerIncrementRoutes requires a service");
    }

    // Перехватываем OPTIONS до маршрутизации: Drogon автоматически отвечает на
    // этот метод пустым 200, но контракт нашего API допускает только POST.
    drogon::app().registerSyncAdvice(
        [](const drogon::HttpRequestPtr& request) -> drogon::HttpResponsePtr {
            if (request->path() != "/api/increment" ||
                request->method() != drogon::Options) {
                return {};
            }

            logging::write(
                "ERROR code=METHOD_NOT_ALLOWED method=OPTIONS path=/api/increment");
            return errorResponse(drogon::k405MethodNotAllowed,
                                 "METHOD_NOT_ALLOWED",
                                 "Only HTTP POST is accepted by this endpoint.");
        });

    drogon::app().registerHandler(
        "/api/increment",
        [service = std::move(service)](
            const drogon::HttpRequestPtr& request,
            std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            auto responder = std::make_shared<ResponseOnce>(std::move(callback));

            if (request->method() != drogon::Post) {
                logging::write("ERROR code=METHOD_NOT_ALLOWED method=" +
                               drogon::to_string(request->method()) +
                               " path=/api/increment");
                responder->send(errorResponse(
                    drogon::k405MethodNotAllowed, "METHOD_NOT_ALLOWED",
                    "Only HTTP POST is accepted by this endpoint."));
                return;
            }

            std::int64_t number = 0;
            if (!parseNumber(request->body(), number)) {
                logging::write(
                    "ERROR code=INVALID_NUMBER message=\"expected a non-negative decimal integer in the request body\"");
                responder->send(errorResponse(
                    drogon::k400BadRequest, "INVALID_NUMBER",
                    "Send one non-negative base-10 integer in the request body."));
                return;
            }

            service->processNumber(number, [responder](const ProcessResult& result) {
                handleProcessResult(result, responder);
            });
        });

    // Проверяем, что HTTP-процесс отвечает. Этот маршрут не проверяет БД:
    // работоспособность запросов к данным дополнительно подтверждают тесты/логи.
    drogon::app().registerHandler(
        "/healthz",
        [](const drogon::HttpRequestPtr&,
           std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            Json::Value body;
            body["status"] = "ok";
            callback(drogon::HttpResponse::newHttpJsonResponse(body));
        },
        {drogon::Get});
}

}  // namespace linear_service
