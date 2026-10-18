#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace linear_service {

// Описываем итог сценария независимо от HTTP-кодов: контроллер сам сопоставляет
// эти статусы с протоколом, а модульные тесты проверяют их без веб-сервера.
enum class ProcessStatus {
    Success,
    DuplicateNumber,
    PredecessorOfProcessedNumber,
    InvalidNumber,
    NumberOutOfRange,
    DatabaseError
};

// Передаём контроллеру все данные, нужные для ответа, не возвращая типы Drogon/SQL.
struct ProcessResult {
    ProcessStatus status{ProcessStatus::DatabaseError};
    std::int64_t input{0};
    std::int64_t result{0};
    std::int64_t related_number{0};
    std::string details;
};

// Используем callback, поскольку запросы к реальной БД выполняются асинхронно.
using ProcessCallback = std::function<void(const ProcessResult&)>;

struct RepositoryError {
    // Сохраняем стабильный код этапа без привязки к типу ошибки конкретного драйвера.
    std::string stage;
    std::string message;
};

}  // namespace linear_service
