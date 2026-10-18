#pragma once

#include "number_repository.hpp"
#include "number_service_interface.hpp"

#include <cstdint>
#include <memory>

namespace linear_service {

// Собираем правила приложения отдельно от HTTP и конкретной СУБД. Благодаря
// внедряемым интерфейсам сервис можно проверять модульными тестами.
class NumberService final : public INumberService {
public:
    // Передаём сервису хранилище через интерфейс и фиксируем допустимую границу n.
    NumberService(std::shared_ptr<INumberRepository> repository,
                  std::int64_t maximum_input);

    // Запускаем сценарий обработки; результат приходит через callback, потому что
    // реальное хранилище работает асинхронно.
    void processNumber(std::int64_t number, ProcessCallback callback) override;

private:
    std::shared_ptr<INumberRepository> repository_;
    std::int64_t maximum_input_;
};

}  // namespace linear_service
