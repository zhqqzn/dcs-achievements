#include "increment_controller.hpp"
#include "number_service.hpp"
#include "postgres_number_repository.hpp"
#include "stdout_logger.hpp"

#include <drogon/drogon.h>

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace {

// Читаем настройку из окружения и сохраняем значения по умолчанию для
// локального запуска, если переменная не задана или пуста.
std::string envOr(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    return value == nullptr || *value == '\0' ? fallback : value;
}

// Ограничиваем вход так, чтобы вычисление n + 1 оставалось в пределах
// знакового int64_t и могло храниться в PostgreSQL BIGINT.
std::int64_t parseMaximumInput() {
    const auto raw = envOr("MAX_INPUT", "9223372036854775806");
    std::int64_t value = 0;
    const auto parsed = std::from_chars(raw.data(), raw.data() + raw.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != raw.data() + raw.size() ||
        value < 0 || value >= std::numeric_limits<std::int64_t>::max()) {
        throw std::runtime_error(
            "MAX_INPUT must be an integer from 0 to 9223372036854775806");
    }
    return value;
}

// Проверяем настройки порта при запуске, чтобы обнаружить ошибку конфигурации
// сразу, а не превращать её позже в непонятную ошибку подключения.
unsigned short parsePort(const std::string& raw) {
    unsigned int value = 0;
    const auto parsed = std::from_chars(raw.data(), raw.data() + raw.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != raw.data() + raw.size() ||
        value == 0 || value > 65535) {
        throw std::runtime_error("DB_PORT must be an integer from 1 to 65535");
    }
    return static_cast<unsigned short>(value);
}

}  // namespace

// Собираем приложение в одной точке: выбираем конкретные адаптеры, передаём
// их сервису через интерфейсы и запускаем HTTP-сервер Drogon.
int main() {
    try {
        const auto maximum_input = parseMaximumInput();

        Json::Value config(Json::objectValue);
        Json::Value listeners(Json::arrayValue);
        Json::Value listener(Json::objectValue);
        listener["address"] = "0.0.0.0";
        listener["port"] = 8080;
        listeners.append(listener);
        config["listeners"] = listeners;

        Json::Value db_clients(Json::arrayValue);
        Json::Value db(Json::objectValue);
        db["name"] = "default";
        db["rdbms"] = "postgresql";
        db["host"] = envOr("DB_HOST", "database");
        db["port"] = static_cast<Json::UInt>(parsePort(envOr("DB_PORT", "5432")));
        db["dbname"] = envOr("DB_NAME", "linear");
        db["user"] = envOr("DB_USER", "linear");
        db["passwd"] = envOr("DB_PASSWORD", "coursework_demo_password");
        // Задаём ограниченный пул соединений и тайм-аут ожидания; эти значения нужны
        // для воспроизводимого примера, а не объявляются оптимальными для любой нагрузки.
        db["number_of_connections"] = 16;
        db["is_fast"] = false;
        db["timeout"] = 5.0;
        db_clients.append(db);
        config["db_clients"] = db_clients;

        // Запускаем восемь рабочих потоков; уровень логов самого Drogon оставляем на WARN,
        // а бизнес-события записываем отдельно через stdout_logger.
        drogon::app()
            .loadConfigJson(config)
            .setThreadNum(8)
            .setLogLevel(trantor::Logger::kWarn);

        auto repository = std::make_shared<linear_service::PostgresNumberRepository>(
            [] { return drogon::app().getDbClient(); });
        auto service = std::make_shared<linear_service::NumberService>(
            repository, maximum_input);
        linear_service::registerIncrementRoutes(std::move(service));

        linear_service::logging::write(
            "INFO service=linear-service listening=0.0.0.0:8080 maximum_input=" +
            std::to_string(maximum_input));
        drogon::app().run();
    } catch (const std::exception& error) {
        linear_service::logging::write(
            "FATAL code=STARTUP_FAILED detail=\"" +
            linear_service::logging::escapeField(error.what()) + "\"");
        return 1;
    }
    return 0;
}
