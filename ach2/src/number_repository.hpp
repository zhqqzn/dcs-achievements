#pragma once

#include "number_types.hpp"

#include <cstdint>
#include <functional>
#include <memory>

namespace linear_service {

// Задаём контракт транзакционного хранения для бизнес-сервиса. Все операции
// одной обработки выполняются в одной транзакции; блокировка состояния должна
// сериализовать проверку и запись между экземплярами приложения, обращающимися
// к одному основному узлу БД. commit() сообщает, удалось ли сохранить изменения.
class INumberTransaction {
public:
    using ExistsCallback = std::function<void(bool)>;
    using CompletionCallback = std::function<void()>;
    using ErrorCallback = std::function<void(RepositoryError)>;
    using CommitCallback = std::function<void(bool, RepositoryError)>;

    virtual ~INumberTransaction() = default;

    // Захватываем общую строку состояния до конца транзакции; false означает, что
    // обязательная строка не найдена и безопасно продолжать обработку нельзя.
    virtual void lockProcessingState(ExistsCallback callback,
                                     ErrorCallback onError) = 0;
    // Проверяем историю всех допустимых входов — не только успешно обработанных.
    virtual void hasReceived(std::int64_t number, ExistsCallback callback,
                             ErrorCallback onError) = 0;
    // Фиксируем первое допустимое поступление, даже если далее сработает исключение № 2.
    virtual void recordReceived(std::int64_t number, CompletionCallback callback,
                                ErrorCallback onError) = 0;
    // Проверяем именно множество успешных обработок для реализации исключения № 2.
    virtual void hasProcessed(std::int64_t number, ExistsCallback callback,
                              ErrorCallback onError) = 0;
    // Записываем вход как успешно обработанный только после всех проверок.
    virtual void recordProcessed(std::int64_t number, CompletionCallback callback,
                                 ErrorCallback onError) = 0;
    // Подтверждаем сохранение до того, как сервис сообщит результат клиенту.
    virtual void commit(CommitCallback callback) = 0;
    // Отменяем незавершённые изменения при ошибке, чтобы не оставить частичное состояние.
    virtual void rollback() noexcept = 0;
};

class INumberRepository {
public:
    using TransactionPtr = std::shared_ptr<INumberTransaction>;
    using TransactionCallback = std::function<void(TransactionPtr)>;
    using ErrorCallback = std::function<void(RepositoryError)>;

    virtual ~INumberRepository() = default;
    // Создаём единый контекст для проверок и записи, чтобы они были атомарными.
    virtual void beginTransaction(TransactionCallback callback,
                                  ErrorCallback onError) = 0;
};

}  // namespace linear_service
