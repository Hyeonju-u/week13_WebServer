-- wrk용 스크립트: 응답 상태가 200이고 본문 길이가 기대값(BENCH_BODY_BYTES)과 같은지 센다.
-- response()를 쓰면 wrk가 느려지므로 처리량을 재는 실행에는 쓰지 않고, bench.sh가 짧은 별도 실행에서만 쓴다.
local expected = tonumber(os.getenv("BENCH_BODY_BYTES") or "0")
local counter = 1
local threads = {}

function setup(thread)
  thread:set("id", counter)
  table.insert(threads, thread)
  counter = counter + 1
end

function init(args)
  checked = 0
  wrong = 0
end

function response(status, headers, body)
  checked = checked + 1
  if status ~= 200 or #body ~= expected then
    wrong = wrong + 1
  end
end

function done(summary, latency, requests)
  local checked_total = 0
  local wrong_total = 0
  for _, thread in ipairs(threads) do
    checked_total = checked_total + thread:get("checked")
    wrong_total = wrong_total + thread:get("wrong")
  end
  io.write(string.format("본문 검사 %d건, 불일치 %d건\n", checked_total, wrong_total))
end
