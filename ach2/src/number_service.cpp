#include "number_service.hpp"

#include "stdout_logger.hpp"

#include <atomic>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace linear_service {
namespace {

// Храним состояние одной операции между асинхронными ответами БД: локальных
// переменных processNumber() недостаточно, потому что callback вызывается позже.
struct OperationState {
    OperationState(std::int64_t value, ProcessCallback callback_value)
        : callback(std::move(callback_value)) {
        result.input = value;
    }

    ProcessResult result;
    ProcessCallback callback;
    INumberRepository::TransactionPtr transaction;
    std::atomic_bool completed{false};
};

// Завершаем операцию при ошибке хранения единообразно: не продолжаем цепочку,
// откатываем незавершённую транзакцию, пишем причину в stdout и отвечаем сервисным
// результатом. Флаг не даёт нескольким callback завершить запрос дважды.
void deliverDatabaseFailure(const std::shared_ptr<OperationState>& state,
                            RepositoryError error) {
    if (state->completed.exchange(true)) {
        return;
    }

    if (state->transaction) {
        state->transaction->rollback();
    }

    std::ostringstream line;
    line << "ERROR code=";
    if (error.stage == "begin_transaction") {
        line << "DATABASE_UNAVAILABLE";
    } else if (error.stage == "request_state_row_missing") {
        line << "DATABASE_SCHEMA_INVALID";
    } else if (error.stage == "commit_transaction") {
        line << "DATABASE_COMMIT_FAILED";
    } else {
        line << "DATABASE_OPERATION_FAILED";
    }
    line << " input=" << state->result.input
         << " stage=" << error.stage
         << " detail=\"" << logging::escapeField(error.message) << "\"";
    logging::write(line.str());

    ProcessResult result;
    result.status = ProcessStatus::DatabaseError;
    result.input = state->result.input;
    result.details = error.stage;
    state->callback(result);
}

// Записываем принятое бизнес-решение одним событием. Ошибки БД журналируем
// отдельно в deliverDatabaseFailure(), где известен этап отказа.
void logBusinessResult(const ProcessResult& result) {
    std::ostringstream line;
    switch (result.status) {
        case ProcessStatus::Success:
            line << "INFO code=NUMBER_PROCESSED input=" << result.input
                 << " result=" << result.result;
            break;
        case ProcessStatus::DuplicateNumber:
            line << "ERROR code=DUPLICATE_NUMBER input=" << result.input
                 << " message=\"number was already received\"";
            break;
        case ProcessStatus::PredecessorOfProcessedNumber:
            line << "ERROR code=PREDECESSOR_OF_PROCESSED_NUMBER input="
                 << result.input << " already_processed=" << result.related_number
                 << " message=\"input is one less than a processed number\"";
            break;
        case ProcessStatus::InvalidNumber:
            line << "ERROR code=INVALID_NUMBER input=" << result.input
                 << " message=\"negative input is not allowed\"";
            break;
        case ProcessStatus::NumberOutOfRange:
            line << "ERROR code=NUMBER_OUT_OF_RANGE input=" << result.input;
            break;
        case ProcessStatus::DatabaseError:
            return;  // Ошибку БД уже записали там, где она возникла.
    }
    logging::write(line.str());
}

// Отдаём бизнес-результат только после подтверждения фиксации транзакции.
// Иначе клиент мог бы получить успех, который не сохранился в БД; это важно
// и для отказов по правилам, поскольку история входа тоже должна сохраниться.
void commitResult(const std::shared_ptr<OperationState>& state) {
    const auto transaction = state->transaction;
    if (!transaction) {
        deliverDatabaseFailure(
            state, RepositoryError{"commit_transaction", "transaction is unavailable"});
        return;
    }

    transaction->commit([state](bool committed, RepositoryError error) {
        if (!committed) {
            if (error.stage.empty()) {
                error.stage = "commit_transaction";
            }
            deliverDatabaseFailure(state, std::move(error));
            return;
        }

        if (state->completed.exchange(true)) {
            return;
        }
        logBusinessResult(state->result);
        state->callback(state->result);
    });
}

void insertProcessedNumber(const std::shared_ptr<OperationState>& state);

// Реализуем исключение № 2: после проверки повторов ищем уже обработанное n + 1.
// Верхняя граница входа гарантирует, что вычисление n + 1 не переполнит int64_t.
void checkProcessedSuccessor(const std::shared_ptr<OperationState>& state) {
    const auto successor = state->result.input + 1;
    state->transaction->hasProcessed(
        successor,
        [state, successor](bool already_processed) {
            if (already_processed) {
                state->result.status = ProcessStatus::PredecessorOfProcessedNumber;
                state->result.related_number = successor;
                commitResult(state);
                return;
            }
            insertProcessedNumber(state);
        },
        [state](RepositoryError error) {
            deliverDatabaseFailure(state, std::move(error));
        });
}

// Сохраняем первое поступление до проверки исключения № 2: поэтому при повторной
// отправке отклонённого предшественника сработает исключение № 1.
void insertRequestHistory(const std::shared_ptr<OperationState>& state) {
    state->transaction->recordReceived(
        state->result.input,
        [state]() { checkProcessedSuccessor(state); },
        [state](RepositoryError error) {
            deliverDatabaseFailure(state, std::move(error));
        });
}

// Проверяем исключение № 1 раньше исключения № 2: повтор уже известного входа
// всегда имеет приоритет, независимо от результата предыдущей попытки.
void checkDuplicate(const std::shared_ptr<OperationState>& state) {
    state->transaction->hasReceived(
        state->result.input,
        [state](bool already_received) {
            if (already_received) {
                state->result.status = ProcessStatus::DuplicateNumber;
                commitResult(state);
                return;
            }
            insertRequestHistory(state);
        },
        [state](RepositoryError error) {
            deliverDatabaseFailure(state, std::move(error));
        });
}

// Помечаем число успешно обработанным только после обеих проверок исключений.
void insertProcessedNumber(const std::shared_ptr<OperationState>& state) {
    state->transaction->recordProcessed(
        state->result.input,
        [state]() {
            state->result.status = ProcessStatus::Success;
            state->result.result = state->result.input + 1;
            commitResult(state);
        },
        [state](RepositoryError error) {
            deliverDatabaseFailure(state, std::move(error));
        });
}

}  // namespace

NumberService::NumberService(std::shared_ptr<INumberRepository> repository,
                             std::int64_t maximum_input)
    : repository_(std::move(repository)), maximum_input_(maximum_input) {
    if (!repository_) {
        throw std::invalid_argument("NumberService requires a repository");
    }
    if (maximum_input_ < 0 ||
        maximum_input_ >= std::numeric_limits<std::int64_t>::max()) {
        throw std::invalid_argument(
            "maximum_input must be from 0 to INT64_MAX - 1");
    }
}

// Выполняем сценарий обработки независимо от HTTP и типа БД: сначала проверяем
// диапазон, затем проводим проверки и изменения в одной транзакции.
void NumberService::processNumber(std::int64_t number, ProcessCallback callback) {
    if (!callback) {
        return;
    }
    if (number < 0) {
        ProcessResult result;
        result.status = ProcessStatus::InvalidNumber;
        result.input = number;
        logBusinessResult(result);
        callback(result);
        return;
    }
    if (number > maximum_input_) {
        ProcessResult result;
        result.status = ProcessStatus::NumberOutOfRange;
        result.input = number;
        logBusinessResult(result);
        callback(result);
        return;
    }

    auto state = std::make_shared<OperationState>(number, std::move(callback));
    repository_->beginTransaction(
        [state](INumberRepository::TransactionPtr transaction) {
            if (!transaction) {
                deliverDatabaseFailure(
                    state, RepositoryError{"begin_transaction", "repository returned no transaction"});
                return;
            }
            state->transaction = std::move(transaction);
            state->transaction->lockProcessingState(
                [state](bool state_row_exists) {
                    if (!state_row_exists) {
                        deliverDatabaseFailure(
                            state, RepositoryError{"request_state_row_missing",
                                                  "required request_state row is missing"});
                        return;
                    }
                    checkDuplicate(state);
                },
                [state](RepositoryError error) {
                    deliverDatabaseFailure(state, std::move(error));
                });
        },
        [state](RepositoryError error) {
            if (error.stage.empty()) {
                error.stage = "begin_transaction";
            }
            deliverDatabaseFailure(state, std::move(error));
        });
}

}  // namespace linear_service
