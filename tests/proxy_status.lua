-- wrk용 스크립트: 응답 상태 코드별 건수를 센다. capacity_test.py가 쓴다.
-- 스레드마다 따로 세므로 끝에 합친다.
local counter = 1
local threads = {}

function setup(thread)
  thread:set("id", counter)
  table.insert(threads, thread)
  counter = counter + 1
end

function init(args)
  codes = {}
end

function response(status, headers, body)
  codes[status] = (codes[status] or 0) + 1
end

function done(summary, latency, requests)
  local total = {}
  for _, thread in ipairs(threads) do
    for code, count in pairs(thread:get("codes")) do
      total[code] = (total[code] or 0) + count
    end
  end
  for code, count in pairs(total) do
    io.write(string.format("상태 %s: %d건\n", tostring(code), count))
  end
end
