#include "number_service.hpp"

#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using namespace linear_service;

// Храним состояние тестовой БД в памяти, чтобы проверять бизнес-правила без PostgreSQL.
struct MemoryState {
    std::set<std::int64_t> received;
    std::set<std::int64_t> processed;
    bool state_row_exists{true};
};

// Имитируем транзакцию: держим mutex до commit/rollback и применяем копию состояния
// только при commit, чтобы тесты сохраняли смысл атомарности.
class MemoryTransaction final : public INumberTransaction {
public:
    MemoryTransaction(std::shared_ptr<MemoryState> database, std::mutex& mutex)
        : database_(std::move(database)), lock_(mutex),
          received_(database_->received), processed_(database_->processed),
          state_row_exists_(database_->state_row_exists) {}

    void lockProcessingState(ExistsCallback callback,
                             ErrorCallback /* onError */) override {
        callback(state_row_exists_);
    }

    void hasReceived(std::int64_t number, ExistsCallback callback,
                     ErrorCallback /* onError */) override {
        callback(received_.count(number) != 0);
    }

    void recordReceived(std::int64_t number, CompletionCallback callback,
                        ErrorCallback /* onError */) override {
        received_.insert(number);
        callback();
    }

    void hasProcessed(std::int64_t number, ExistsCallback callback,
                      ErrorCallback /* onError */) override {
        callback(processed_.count(number) != 0);
    }

    void recordProcessed(std::int64_t number, CompletionCallback callback,
                         ErrorCallback /* onError */) override {
        processed_.insert(number);
        callback();
    }

    void commit(CommitCallback callback) override {
        if (!active_) {
            callback(false, RepositoryError{"commit_transaction", "transaction is inactive"});
            return;
        }
        database_->received = received_;
        database_->processed = processed_;
        active_ = false;
        lock_.unlock();
        callback(true, RepositoryError{});
    }

    void rollback() noexcept override {
        if (active_) {
            active_ = false;
            if (lock_.owns_lock()) {
                lock_.unlock();
            }
        }
    }

private:
    std::shared_ptr<MemoryState> database_;
    std::unique_lock<std::mutex> lock_;
    std::set<std::int64_t> received_;
    std::set<std::int64_t> processed_;
    bool state_row_exists_{true};
    bool active_{true};
};

// Подменяем PostgreSQL тестовым хранилищем через тот же интерфейс, что использует сервис.
class MemoryRepository final : public INumberRepository {
public:
    explicit MemoryRepository(bool state_row_exists = true)
        : state_(std::make_shared<MemoryState>()) {
        state_->state_row_exists = state_row_exists;
    }

    void beginTransaction(TransactionCallback callback,
                          ErrorCallback /* onError */) override {
        callback(std::make_shared<MemoryTransaction>(state_, mutex_));
    }

private:
    std::shared_ptr<MemoryState> state_;
    std::mutex mutex_;
};

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// Вызываем сценарий одинаково во всех тестах и проверяем, что callback завершил его ровно один раз.
ProcessResult invoke(INumberService& service, std::int64_t number) {
    bool called = false;
    ProcessResult received;
    service.processNumber(number, [&called, &received](const ProcessResult& result) {
        require(!called, "service callback must be called at most once");
        received = result;
        called = true;
    });
    require(called, "service callback was not called");
    return received;
}

// Проверяем обычную обработку n -> n + 1 и приоритет исключения № 1 при повторе.
void testSuccessAndDuplicate() {
    auto repository = std::make_shared<MemoryRepository>();
    NumberService service(repository, 1000);

    const auto success = invoke(service, 41);
    require(success.status == ProcessStatus::Success, "41 should be processed");
    require(success.result == 42, "result must equal input + 1");

    const auto duplicate = invoke(service, 41);
    require(duplicate.status == ProcessStatus::DuplicateNumber,
            "a repeated input must be exception #1");
}

// Закрепляем правило № 2 и то, что отклонённый вход при следующей отправке становится повтором.
void testPredecessorThenDuplicate() {
    auto repository = std::make_shared<MemoryRepository>();
    NumberService service(repository, 1000);

    const auto processed = invoke(service, 79);
    require(processed.status == ProcessStatus::Success, "79 should be processed");

    const auto predecessor = invoke(service, 78);
    require(predecessor.status == ProcessStatus::PredecessorOfProcessedNumber,
            "78 should be exception #2 after 79 was processed");
    require(predecessor.related_number == 79,
            "exception #2 must identify the processed successor");

    const auto repeated_predecessor = invoke(service, 78);
    require(repeated_predecessor.status == ProcessStatus::DuplicateNumber,
            "the rejected predecessor should become a duplicate on the next request");
}

// Проверяем границы до обращения к хранилищу и корректную реакцию на нарушенное состояние БД.
void testRangeIsRejectedBeforeRepositoryUse() {
    auto repository = std::make_shared<MemoryRepository>(false);
    NumberService service(repository, 12);

    const auto negative = invoke(service, -1);
    require(negative.status == ProcessStatus::InvalidNumber,
            "negative input should be rejected by the service");

    const auto out_of_range = invoke(service, 13);
    require(out_of_range.status == ProcessStatus::NumberOutOfRange,
            "input above maximum should be rejected by the service");

    const auto missing_database_state = invoke(service, 12);
    require(missing_database_state.status == ProcessStatus::DatabaseError,
            "the service should surface an invalid DB state as a repository failure");
}

// Не допускаем верхнюю границу, при которой вычисление n + 1 переполняет int64_t.
void testConstructorRejectsUnsafeMaximum() {
    auto repository = std::make_shared<MemoryRepository>();
    bool rejected = false;
    try {
        NumberService service(repository, std::numeric_limits<std::int64_t>::max());
        (void)service;
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "maximum must leave room for input + 1");
}

}  // namespace

// Запускаем все модульные сценарии и сообщаем сборочной системе о первом нарушении.
int main() {
    try {
        testSuccessAndDuplicate();
        testPredecessorThenDuplicate();
        testRangeIsRejectedBeforeRepositoryUse();
        testConstructorRejectsUnsafeMaximum();
        std::cout << "PASS number-service-unit-tests (4 cases)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL number-service-unit-tests: " << error.what() << '\n';
        return 1;
    }
}
