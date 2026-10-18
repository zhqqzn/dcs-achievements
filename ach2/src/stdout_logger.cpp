#include "stdout_logger.hpp"

#include <iostream>
#include <mutex>

namespace linear_service::logging {
namespace {
std::mutex output_mutex;
}

// Пишем событие целиком и сразу сбрасываем буфер: контейнерный сборщик должен
// получить готовую строку даже при параллельной обработке запросов.
void write(const std::string& line) {
    std::lock_guard<std::mutex> lock(output_mutex);
    std::cout << line << '\n' << std::flush;
}

// Не даём кавычкам и переводам строки из текста ошибки ломать структуру журнала.
std::string escapeField(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char ch : value) {
        switch (ch) {
            case '\\': escaped += "\\\\"; break;
            case '"': escaped += "\\\""; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default: escaped += ch; break;
        }
    }
    return escaped;
}

}  // namespace linear_service::logging
