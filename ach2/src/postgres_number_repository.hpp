#pragma once

#include "number_repository.hpp"

#include <drogon/drogon.h>

#include <functional>
#include <memory>

namespace linear_service {

// Реализуем контракты хранения средствами PostgreSQL и транзакций Drogon. Только
// здесь находятся SQL и детали драйвера; NumberService зависит от интерфейсов.
class PostgresNumberRepository final : public INumberRepository {
public:
    // Описываем способ получить настроенный клиент БД без жёсткой привязки к глобальному объекту.
    using DbClientProvider = std::function<std::shared_ptr<drogon::orm::DbClient>()>;

    // Получаем клиент лениво: при создании адаптера пул соединений может быть ещё не готов.
    explicit PostgresNumberRepository(DbClientProvider db_client_provider);

    // Начинаем транзакцию и возвращаем сервису адаптер над транзакцией Drogon.
    void beginTransaction(TransactionCallback callback,
                          ErrorCallback onError) override;

private:
    DbClientProvider db_client_provider_;
};

}  // namespace linear_service
