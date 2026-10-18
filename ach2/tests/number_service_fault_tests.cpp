#include "number_service.hpp"
#include "stdout_logger.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace linear_service;

// Храним состояние тестовой БД и настройки отказов, чтобы воспроизводить ошибки по этапам.
struct MemoryState {
    std::set<std::int64_t> received;
    std::set<std::int64_t> processed;
    bool state_row_exists{true};
    bool return_null_transaction{false};
    bool repeat_begin_error_callback{false};
    std::string fail_stage;
    std::atomic<int> begin_calls{0};
    std::atomic<int> commit_calls{0};
    std::atomic<int> rollback_calls{0};
};

// Копируем состояние на начало транзакции и фиксируем копию только при успешном commit.
class MemoryTransaction final : public INumberTransaction {
public:
    MemoryTransaction(std::shared_ptr<MemoryState> database, std::mutex& mutex)
        : database_(std::move(database)), lock_(mutex),
          received_(database_->received), processed_(database_->processed),
          state_row_exists_(database_->state_row_exists) {}

    void lockProcessingState(ExistsCallback callback,
                             ErrorCallback onError) override {
        if (fail("lock_processing_state", onError)) return;
        callback(state_row_exists_);
    }

    void hasReceived(std::int64_t number, ExistsCallback callback,
                     ErrorCallback onError) override {
        if (fail("check_request_history", onError)) return;
        callback(received_.count(number) != 0);
    }

    void recordReceived(std::int64_t number, CompletionCallback callback,
                        ErrorCallback onError) override {
        if (fail("insert_request_history", onError)) return;
        received_.insert(number);
        callback();
    }

    void hasProcessed(std::int64_t number, ExistsCallback callback,
                      ErrorCallback onError) override {
        if (fail("check_processed_numbers", onError)) return;
        callback(processed_.count(number) != 0);
    }

    void recordProcessed(std::int64_t number, CompletionCallback callback,
                         ErrorCallback onError) override {
        if (fail("insert_processed_number", onError)) return;
        processed_.insert(number);
        callback();
    }

    void commit(CommitCallback callback) override {
        ++database_->commit_calls;
        if (!active_) {
            callback(false, RepositoryError{
                "commit_transaction", "transaction is inactive"});
            return;
        }
        if (database_->fail_stage == "commit_transaction") {
            callback(false, RepositoryError{
                "commit_transaction", "injected commit failure"});
            return;
        }
        database_->received = received_;
        database_->processed = processed_;
        active_ = false;
        lock_.unlock();
        callback(true, RepositoryError{});
    }

    void rollback() noexcept override {
        if (!active_) return;
        active_ = false;
        ++database_->rollback_calls;
        if (lock_.owns_lock()) lock_.unlock();
    }

private:
    bool fail(const std::string& stage, const ErrorCallback& onError) {
        if (database_->fail_stage != stage) return false;
        onError(RepositoryError{stage, "injected operation failure"});
        return true;
    }

    std::shared_ptr<MemoryState> database_;
    std::unique_lock<std::mutex> lock_;
    std::set<std::int64_t> received_;
    std::set<std::int64_t> processed_;
    bool state_row_exists_{true};
    bool active_{true};
};

// Управляем началом транзакций и внедряем ошибки без запуска PostgreSQL.
class MemoryRepository final : public INumberRepository {
public:
    explicit MemoryRepository(bool state_row_exists = true)
        : state_(std::make_shared<MemoryState>()) {
        state_->state_row_exists = state_row_exists;
    }

    void beginTransaction(TransactionCallback callback,
                          ErrorCallback onError) override {
        ++state_->begin_calls;
        if (state_->fail_stage == "begin_transaction") {
            const RepositoryError error{
                "begin_transaction", "injected transaction creation failure"};
            onError(error);
            if (state_->repeat_begin_error_callback) {
                // Имитируем нарушение контракта адаптера: повторный callback не должен удваивать ответ.
                onError(error);
            }
            return;
        }
        if (state_->return_null_transaction) {
            callback(nullptr);
            return;
        }
        callback(std::make_shared<MemoryTransaction>(state_, mutex_));
    }

    std::shared_ptr<MemoryState> state() const { return state_; }

private:
    std::shared_ptr<MemoryState> state_;
    std::mutex mutex_;
};

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct Invocation {
    ProcessResult result;
    int callback_count{0};
};

Invocation invoke(INumberService& service, std::int64_t number) {
    Invocation invocation;
    service.processNumber(number, [&invocation](const ProcessResult& result) {
        ++invocation.callback_count;
        invocation.result = result;
    });
    require(invocation.callback_count == 1,
            "service callback must be called exactly once synchronously by the memory adapter");
    return invocation;
}

// Повторяем основной сценарий через порт хранилища и проверяем, какие данные фиксируются.
void testSuccessAndDuplicate() {
    auto repository = std::make_shared<MemoryRepository>();
    NumberService service(repository, 1000);

    const auto success = invoke(service, 41);
    require(success.result.status == ProcessStatus::Success, "41 should be processed");
    require(success.result.result == 42, "result must equal input + 1");
    require(repository->state()->received.count(41) == 1,
            "successful input must be recorded in request history");
    require(repository->state()->processed.count(41) == 1,
            "successful input must be recorded as processed");

    const auto duplicate = invoke(service, 41);
    require(duplicate.result.status == ProcessStatus::DuplicateNumber,
            "a repeated input must be exception #1");
    require(repository->state()->commit_calls == 2,
            "both the successful request and duplicate result commit their transactions");
}

// Проверяем, что исключение № 2 регистрирует вход и превращает следующий запрос в исключение № 1.
void testPredecessorThenDuplicate() {
    auto repository = std::make_shared<MemoryRepository>();
    NumberService service(repository, 1000);

    require(invoke(service, 79).result.status == ProcessStatus::Success,
            "79 should be processed");
    const auto predecessor = invoke(service, 78);
    require(predecessor.result.status == ProcessStatus::PredecessorOfProcessedNumber,
            "78 should be exception #2 after 79 was processed");
    require(predecessor.result.related_number == 79,
            "exception #2 must identify the processed successor");
    require(repository->state()->received.count(78) == 1,
            "a rejected predecessor must still be recorded as received");
    require(repository->state()->processed.count(78) == 0,
            "a rejected predecessor must not be recorded as processed");

    const auto repeated = invoke(service, 78);
    require(repeated.result.status == ProcessStatus::DuplicateNumber,
            "the rejected predecessor should become exception #1 on the next request");
}

// Проверяем ранний отказ по диапазону и обнаружение отсутствующей строки состояния.
void testRangeRejectedBeforeRepositoryUse() {
    auto repository = std::make_shared<MemoryRepository>(false);
    NumberService service(repository, 12);

    require(invoke(service, -1).result.status == ProcessStatus::InvalidNumber,
            "negative input should be rejected by the service");
    require(invoke(service, 13).result.status == ProcessStatus::NumberOutOfRange,
            "input above maximum should be rejected by the service");
    require(repository->state()->begin_calls == 0,
            "invalid inputs must not open a database transaction");

    require(invoke(service, 12).result.status == ProcessStatus::DatabaseError,
            "a missing database state row should be surfaced as a repository failure");
    require(repository->state()->rollback_calls == 1,
            "a transaction with invalid state must be rolled back");
}

// Проверяем последнюю допустимую границу и вычисление n + 1 без переполнения.
// Проверяем минимальную допустимую конфигурацию: при MAX_INPUT = 0 число 0 остаётся разрешённым.
void testZeroIsValidConfiguredMaximum() {
    auto repository = std::make_shared<MemoryRepository>();
    NumberService service(repository, 0);

    const auto zero = invoke(service, 0);
    require(zero.result.status == ProcessStatus::Success,
            "zero must remain a valid input when the configured maximum is zero");
    require(zero.result.result == 1, "zero must produce one");

    const auto one = invoke(service, 1);
    require(one.result.status == ProcessStatus::NumberOutOfRange,
            "one must be rejected when the configured maximum is zero");
    require(repository->state()->begin_calls == 1,
            "out-of-range input must not open a second transaction");
}

void testMaximumAllowedInputDoesNotOverflow() {
    auto repository = std::make_shared<MemoryRepository>();
    const auto maximum = std::numeric_limits<std::int64_t>::max() - 1;
    NumberService service(repository, maximum);

    const auto last_allowed = invoke(service, maximum);
    require(last_allowed.result.status == ProcessStatus::Success,
            "INT64_MAX - 1 must be accepted when configured as the maximum");
    require(last_allowed.result.result == std::numeric_limits<std::int64_t>::max(),
            "adding one to the last allowed input must not overflow");

    const auto first_rejected = invoke(service, std::numeric_limits<std::int64_t>::max());
    require(first_rejected.result.status == ProcessStatus::NumberOutOfRange,
            "INT64_MAX must be rejected because its successor is not representable");
    require(repository->state()->begin_calls == 1,
            "the out-of-range value must be rejected before opening a transaction");
}

// Проверяем конфигурационные инварианты сервиса и отсутствие работы при пустом callback.
void testConstructorAndEmptyCallbackContracts() {
    bool null_repository_rejected = false;
    try {
        NumberService service(nullptr, 100);
        (void)service;
    } catch (const std::invalid_argument&) {
        null_repository_rejected = true;
    }
    require(null_repository_rejected, "the service must reject a null repository");

    auto repository = std::make_shared<MemoryRepository>();
    bool unsafe_maximum_rejected = false;
    try {
        NumberService service(repository, std::numeric_limits<std::int64_t>::max());
        (void)service;
    } catch (const std::invalid_argument&) {
        unsafe_maximum_rejected = true;
    }
    require(unsafe_maximum_rejected, "maximum must leave room for input + 1");

    bool negative_maximum_rejected = false;
    try {
        NumberService service(repository, -1);
        (void)service;
    } catch (const std::invalid_argument&) {
        negative_maximum_rejected = true;
    }
    require(negative_maximum_rejected, "maximum input cannot be negative");

    NumberService service(repository, 100);
    service.processNumber(5, ProcessCallback{});
    require(repository->state()->begin_calls == 0,
            "an empty result callback must not start database work");
}

// Проверяем защиту от нарушения контракта, когда адаптер не возвращает транзакцию.
void testNoTransactionReturnedByRepository() {
    auto repository = std::make_shared<MemoryRepository>();
    repository->state()->return_null_transaction = true;
    NumberService service(repository, 100);
    const auto result = invoke(service, 17);
    require(result.result.status == ProcessStatus::DatabaseError,
            "a null transaction must become a database error");
    require(result.result.details == "begin_transaction",
            "a null transaction must retain its failure stage");
    require(repository->state()->rollback_calls == 0,
            "there is no transaction object to roll back");
}

// Проверяем, что ошибка открытия транзакции превращается в результат сбоя хранилища.
void testBeginTransactionFailure() {
    auto repository = std::make_shared<MemoryRepository>();
    repository->state()->fail_stage = "begin_transaction";
    NumberService service(repository, 100);
    const auto result = invoke(service, 17);
    require(result.result.status == ProcessStatus::DatabaseError,
            "transaction creation failure must be surfaced to the caller");
    require(result.result.details == "begin_transaction",
            "transaction creation failure must retain its stage");
    require(repository->state()->rollback_calls == 0,
            "a transaction that never began cannot be rolled back");
}

// Внедряем отказ на каждом шаге записи и убеждаемся, что частичное состояние не фиксируется.
// Защищаем внешний контракт сервиса от повторного callback сбоя со стороны адаптера.
void testRepeatedRepositoryErrorCallbackIsDeliveredOnce() {
    auto repository = std::make_shared<MemoryRepository>();
    repository->state()->fail_stage = "begin_transaction";
    repository->state()->repeat_begin_error_callback = true;
    NumberService service(repository, 1000);

    int callback_count = 0;
    ProcessResult received;
    service.processNumber(31, [&callback_count, &received](const ProcessResult& result) {
        ++callback_count;
        received = result;
    });

    require(callback_count == 1,
            "a duplicate repository error callback must not produce duplicate responses");
    require(received.status == ProcessStatus::DatabaseError,
            "repository callback violations must still report the original database error");
}

void testEachTransactionOperationFailureRollsBack() {
    const std::vector<std::string> stages = {
        "lock_processing_state",
        "check_request_history",
        "insert_request_history",
        "check_processed_numbers",
        "insert_processed_number",
        "commit_transaction",
    };

    for (const auto& stage : stages) {
        auto repository = std::make_shared<MemoryRepository>();
        repository->state()->fail_stage = stage;
        NumberService service(repository, 1000);
        const auto invocation = invoke(service, 43);
        require(invocation.result.status == ProcessStatus::DatabaseError,
                stage + " must become a database error");
        require(invocation.result.details == stage,
                stage + " must be preserved in the result");
        require(repository->state()->rollback_calls == 1,
                stage + " must roll back the active transaction exactly once");
        require(repository->state()->received.count(43) == 0,
                stage + " must not commit a partial request-history row");
        require(repository->state()->processed.count(43) == 0,
                stage + " must not commit a partial processed row");
    }
}

// Проверяем, что ошибка commit не теряет ранее зафиксированную историю повтора.
void testDuplicateCommitFailurePreservesExistingHistory() {
    auto repository = std::make_shared<MemoryRepository>();
    repository->state()->received.insert(77);
    repository->state()->processed.insert(77);
    repository->state()->fail_stage = "commit_transaction";
    NumberService service(repository, 1000);

    const auto result = invoke(service, 77);
    require(result.result.status == ProcessStatus::DatabaseError,
            "a failed commit must not return a duplicate result before transaction outcome is known");
    require(repository->state()->received.count(77) == 1,
            "failed duplicate commit must preserve the previously received input");
    require(repository->state()->processed.count(77) == 1,
            "failed duplicate commit must preserve the previously processed input");
    require(repository->state()->rollback_calls == 1,
            "failed duplicate commit must roll back exactly once");
}

// Проверяем, что неудачный commit откатывает запись отклонённого предшественника.
void testCommitFailureRollsBackPredecessorHistory() {
    auto repository = std::make_shared<MemoryRepository>();
    repository->state()->received.insert(101);
    repository->state()->processed.insert(101);
    repository->state()->fail_stage = "commit_transaction";
    NumberService service(repository, 1000);

    const auto result = invoke(service, 100);
    require(result.result.status == ProcessStatus::DatabaseError,
            "a failed commit must not return exception #2 as if it were durable");
    require(repository->state()->received.count(100) == 0,
            "failed commit must roll back the newly received predecessor");
    require(repository->state()->processed.count(101) == 1,
            "rollback must preserve data committed before this transaction");
    require(repository->state()->rollback_calls == 1,
            "failed predecessor commit must roll back exactly once");
}

// Проверяем откат всей операции, если обязательная строка блокировки отсутствует.
void testMissingSingletonStateRollsBack() {
    auto repository = std::make_shared<MemoryRepository>(false);
    NumberService service(repository, 100);
    const auto result = invoke(service, 9);
    require(result.result.status == ProcessStatus::DatabaseError,
            "missing singleton state must be reported as a database error");
    require(result.result.details == "request_state_row_missing",
            "missing singleton state must have a stable diagnostic stage");
    require(repository->state()->rollback_calls == 1,
            "missing singleton state must roll back the transaction");
    require(repository->state()->received.empty() && repository->state()->processed.empty(),
            "missing singleton state must not persist request data");
}

// Убеждаемся, что при конкуренции одно значение успешно фиксируется не более одного раза.
void testConcurrentDuplicateRequests() {
    auto repository = std::make_shared<MemoryRepository>();
    NumberService service(repository, 1000);
    constexpr int request_count = 8;
    std::atomic<int> successes{0};
    std::atomic<int> duplicates{0};
    std::vector<std::thread> threads;
    threads.reserve(request_count);

    for (int index = 0; index < request_count; ++index) {
        threads.emplace_back([&service, &successes, &duplicates]() {
            const auto result = invoke(service, 600);
            if (result.result.status == ProcessStatus::Success) {
                ++successes;
            } else if (result.result.status == ProcessStatus::DuplicateNumber) {
                ++duplicates;
            }
        });
    }
    for (auto& thread : threads) thread.join();

    require(successes == 1, "one concurrent duplicate request must succeed");
    require(duplicates == request_count - 1,
            "all remaining concurrent duplicates must be rejected");
}

// Экранируем управляющие символы, чтобы пользовательские данные не подделывали структуру журнала.
void testLogFieldEscaping() {
    const std::string input = "quote=\" slash=\\ line=\nreturn=\rtab=\t";
    const std::string expected = "quote=\\\" slash=\\\\ line=\\nreturn=\\rtab=\\t";
    require(logging::escapeField(input) == expected,
            "log field escaping must prevent quotes and control characters breaking a line");
}

// Проверяем, что параллельные потоки пишут целые строки без взаимного перемешивания.
void testConcurrentLogWritesRemainWholeLines() {
    std::ostringstream captured;
    auto* original_buffer = std::cout.rdbuf(captured.rdbuf());
    constexpr int thread_count = 8;
    constexpr int lines_per_thread = 100;
    std::vector<std::thread> threads;
    threads.reserve(thread_count);

    for (int thread_index = 0; thread_index < thread_count; ++thread_index) {
        threads.emplace_back([thread_index]() {
            for (int line_index = 0; line_index < lines_per_thread; ++line_index) {
                logging::write("test-thread=" + std::to_string(thread_index) +
                               " line=" + std::to_string(line_index));
            }
        });
    }
    for (auto& thread : threads) thread.join();
    std::cout.rdbuf(original_buffer);

    std::istringstream lines(captured.str());
    std::string line;
    std::size_t line_count = 0;
    while (std::getline(lines, line)) {
        require(line.rfind("test-thread=", 0) == 0 && line.find(" line=") != std::string::npos,
                "concurrent logger output must not contain interleaved fragments");
        ++line_count;
    }
    require(line_count == static_cast<std::size_t>(thread_count * lines_per_thread),
            "every concurrent log call must produce exactly one complete line");
}

void run(const std::string& name, const std::function<void()>& test, int& count) {
    test();
    ++count;
    std::cout << "PASS " << name << '\n';
}

}  // namespace

int main() {
    try {
        int count = 0;
        run("success-and-duplicate", testSuccessAndDuplicate, count);
        run("predecessor-then-duplicate", testPredecessorThenDuplicate, count);
        run("range-before-repository", testRangeRejectedBeforeRepositoryUse, count);
        run("zero-configured-maximum", testZeroIsValidConfiguredMaximum, count);
        run("maximum-input-without-overflow", testMaximumAllowedInputDoesNotOverflow, count);
        run("constructor-and-callback-contracts", testConstructorAndEmptyCallbackContracts, count);
        run("null-transaction", testNoTransactionReturnedByRepository, count);
        run("begin-transaction-failure", testBeginTransactionFailure, count);
        run("repository-error-callback-once", testRepeatedRepositoryErrorCallbackIsDeliveredOnce, count);
        run("transaction-operation-failures-rollback", testEachTransactionOperationFailureRollsBack, count);
        run("duplicate-commit-failure-preserves-history", testDuplicateCommitFailurePreservesExistingHistory, count);
        run("predecessor-commit-failure-rollback", testCommitFailureRollsBackPredecessorHistory, count);
        run("missing-state-row-rollback", testMissingSingletonStateRollsBack, count);
        run("concurrent-duplicate-requests", testConcurrentDuplicateRequests, count);
        run("log-field-escaping", testLogFieldEscaping, count);
        run("concurrent-log-writes", testConcurrentLogWritesRemainWholeLines, count);
        std::cout << "PASS number-service-unit-tests (" << count << " cases)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL number-service-unit-tests: " << error.what() << '\n';
        return 1;
    }
}
