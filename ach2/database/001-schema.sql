CREATE TABLE IF NOT EXISTS request_state (
    id SMALLINT PRIMARY KEY CHECK (id = 1)
);

INSERT INTO request_state (id)
VALUES (1)
ON CONFLICT (id) DO NOTHING;

-- Фиксируем каждое корректное число в диапазоне ровно один раз.
-- Даже вход, отклонённый исключением № 2, при следующей отправке должен считаться повтором.
CREATE TABLE IF NOT EXISTS request_history (
    input_value BIGINT PRIMARY KEY CHECK (input_value >= 0),
    first_seen_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
);

-- Здесь храним только числа, которые прошли обе проверки и были успешно обработаны.
CREATE TABLE IF NOT EXISTS processed_numbers (
    input_value BIGINT PRIMARY KEY CHECK (input_value >= 0),
    processed_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
);

