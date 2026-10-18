#pragma once

#include "number_types.hpp"

namespace linear_service {

// Описываем контракт бизнес-сценария без зависимости от HTTP, Drogon и СУБД.
class INumberService {
public:
    virtual ~INumberService() = default;
    // Принимаем число и возвращаем бизнес-результат, а не HTTP-ответ или объект БД.
    virtual void processNumber(std::int64_t number, ProcessCallback callback) = 0;
};

}  // namespace linear_service
