#pragma once

#include "number_service_interface.hpp"

#include <memory>

namespace linear_service {

// Регистрируем HTTP-маршруты, передавая им сервис через его интерфейс.
void registerIncrementRoutes(std::shared_ptr<INumberService> service);

}  // namespace linear_service
