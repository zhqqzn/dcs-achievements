#include "postgres_number_repository.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace linear_service {
namespace {

using drogon::orm::DrogonDbException;
using drogon::orm::Result;
using drogon::orm::Transaction;
using DrogonTransactionPtr = std::shared_ptr<Transaction>;

// Преобразуем исключение драйвера в общий тип ошибки, чтобы не выпускать Drogon
// за пределы инфраструктурного адаптера.
RepositoryError makeError(const std::string& stage,
                          const DrogonDbException& error) {
    return RepositoryError{stage, error.base().what()};
}

class PostgresNumberTransaction final : public INumberTransaction {
public:
    explicit PostgresNumberTransaction(DrogonTransactionPtr transaction)
        : transaction_(std::move(transaction)) {}

    // Блокируем единственную строку состояния на время транзакции: так проверка и
    // запись сериализуются даже при нескольких экземплярах сервера приложений.
    void lockProcessingState(ExistsCallback callback,
                             ErrorCallback onError) override {
        if (!ensureActive(onError, "lock_processing_state")) {
            return;
        }
        transaction_->execSqlAsync(
            "SELECT id FROM request_state WHERE id = 1 FOR UPDATE",
            [callback = std::move(callback)](const Result& rows) {
                callback(!rows.empty());
            },
            [onError = std::move(onError)](const DrogonDbException& error) {
                onError(makeError("lock_processing_state", error));
            });
    }

    // Ищем число в истории входов; таблица включает и запросы, отклонённые правилом № 2.
    void hasReceived(std::int64_t number, ExistsCallback callback,
                     ErrorCallback onError) override {
        if (!ensureActive(onError, "check_request_history")) {
            return;
        }
        transaction_->execSqlAsync(
            "SELECT 1 FROM request_history WHERE input_value = $1",
            [callback = std::move(callback)](const Result& rows) {
                callback(!rows.empty());
            },
            [onError = std::move(onError)](const DrogonDbException& error) {
                onError(makeError("check_request_history", error));
            },
            number);
    }

    // Сохраняем вход параметризованным запросом, не подставляя пользовательские данные
    // прямо в SQL-текст.
    void recordReceived(std::int64_t number, CompletionCallback callback,
                       ErrorCallback onError) override {
        if (!ensureActive(onError, "insert_request_history")) {
            return;
        }
        transaction_->execSqlAsync(
            "INSERT INTO request_history (input_value) VALUES ($1)",
            [callback = std::move(callback)](const Result&) { callback(); },
            [onError = std::move(onError)](const DrogonDbException& error) {
                onError(makeError("insert_request_history", error));
            },
            number);
    }

    // Ищем только в таблице успешных обработок для правила «n + 1 уже обработано».
    void hasProcessed(std::int64_t number, ExistsCallback callback,
                      ErrorCallback onError) override {
        if (!ensureActive(onError, "check_processed_numbers")) {
            return;
        }
        transaction_->execSqlAsync(
            "SELECT 1 FROM processed_numbers WHERE input_value = $1",
            [callback = std::move(callback)](const Result& rows) {
                callback(!rows.empty());
            },
            [onError = std::move(onError)](const DrogonDbException& error) {
                onError(makeError("check_processed_numbers", error));
            },
            number);
    }

    // Сохраняем число как успешное только после проверок повторов и предшественника.
    void recordProcessed(std::int64_t number, CompletionCallback callback,
                         ErrorCallback onError) override {
        if (!ensureActive(onError, "insert_processed_number")) {
            return;
        }
        transaction_->execSqlAsync(
            "INSERT INTO processed_numbers (input_value) VALUES ($1)",
            [callback = std::move(callback)](const Result&) { callback(); },
            [onError = std::move(onError)](const DrogonDbException& error) {
                onError(makeError("insert_processed_number", error));
            },
            number);
    }

    // Сообщаем сервису реальный результат фиксации: до callback нельзя считать
    // изменение долговечным и возвращать клиенту окончательный бизнес-ответ.
    void commit(CommitCallback callback) override {
        if (!transaction_) {
            callback(false, RepositoryError{
                "commit_transaction", "transaction is no longer active"});
            return;
        }

        auto transaction = std::move(transaction_);
        auto callback_holder = std::make_shared<CommitCallback>(std::move(callback));
        try {
            transaction->setCommitCallback(
                [callback_holder](bool committed) {
                    if (committed) {
                        (*callback_holder)(true, RepositoryError{});
                    } else {
                        (*callback_holder)(false, RepositoryError{
                            "commit_transaction", "database transaction commit failed"});
                    }
                });
            // В Drogon сброс последней shared_ptr к Transaction завершает транзакцию;
            // установленный callback сообщает, чем закончилась фиксация.
            transaction.reset();
        } catch (const std::exception& error) {
            (*callback_holder)(false, RepositoryError{
                "commit_transaction", error.what()});
            transaction.reset();
        }
    }

    // Явно отменяем транзакцию при ошибке и освобождаем принадлежащую нам ссылку.
    void rollback() noexcept override {
        auto transaction = std::move(transaction_);
        if (!transaction) {
            return;
        }
        try {
            transaction->rollback();
        } catch (...) {
            // Сохраняем исходную ошибку: вторичное исключение при rollback не должно её скрыть.
        }
        transaction.reset();
    }

private:
    // Не отправляем SQL в уже завершённую транзакцию: сообщаем об ошибке через общий контракт.
    bool ensureActive(const ErrorCallback& onError, const std::string& stage) {
        if (transaction_) {
            return true;
        }
        onError(RepositoryError{stage, "transaction is no longer active"});
        return false;
    }

    DrogonTransactionPtr transaction_;
};

}  // namespace

PostgresNumberRepository::PostgresNumberRepository(DbClientProvider db_client_provider)
    : db_client_provider_(std::move(db_client_provider)) {
    if (!db_client_provider_) {
        throw std::invalid_argument(
            "PostgresNumberRepository requires a DB client provider");
    }
}

// Открываем транзакцию асинхронно и переводим ошибки получения клиента/транзакции
// в RepositoryError, чтобы сервис не зависел от исключений Drogon.
void PostgresNumberRepository::beginTransaction(TransactionCallback callback,
                                                ErrorCallback onError) {
    std::shared_ptr<drogon::orm::DbClient> db_client;
    try {
        db_client = db_client_provider_();
    } catch (const std::exception& error) {
        onError(RepositoryError{"begin_transaction", error.what()});
        return;
    }
    if (!db_client) {
        onError(RepositoryError{
            "begin_transaction", "configured database client is unavailable"});
        return;
    }

    try {
        // Все последующие запросы этого сценария выполняем через один объект транзакции.
        db_client->newTransactionAsync(
            [callback = std::move(callback), onError](
                const DrogonTransactionPtr& transaction) mutable {
                if (!transaction) {
                    onError(RepositoryError{
                        "begin_transaction", "database did not provide a transaction"});
                    return;
                }
                callback(std::make_shared<PostgresNumberTransaction>(transaction));
            });
    } catch (const std::exception& error) {
        onError(RepositoryError{"begin_transaction", error.what()});
    }
}

}  // namespace linear_service
