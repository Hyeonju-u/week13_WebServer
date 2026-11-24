const cases = [
  { id: "static", title: "정적 HTML", description: "서버가 보낸 HTML 태그를 읽고, 본문의 바이트 수를 Content-Length와 비교합니다.",
    method: "GET", path: "/sample.html", expected: 200 },
  { id: "echo", title: "JSON 에코", description: "메시지를 바꿔 보내고, 돌아온 JSON이 보낸 내용과 같은지 확인합니다.",
    method: "POST", path: "/api/echo", expected: 200 },
  { id: "missing", title: "없는 파일", description: "파일을 찾지 못했을 때의 상태 코드와 본문을 확인합니다.",
    method: "GET", path: "/missing-example.txt", expected: 404 },
  { id: "method", title: "미지원 메서드", description: "PUT 요청을 보낸 뒤 응답 헤더의 Allow에서 지원하는 메서드를 확인합니다.",
    method: "PUT", path: "/sample.txt", expected: 405 },
  { id: "parallel", title: "동시 요청", description: "같은 파일에 요청 세 개를 함께 보내 각각의 상태와 소요 시간을 확인합니다.",
    method: "GET", path: "/sample.txt", expected: 200 },
  { id: "proxy", title: "리버스 프록시", description: "다른 터미널에서 make backend를 실행한 뒤 요청하세요. 기본 /proxy 경로로 받은 요청을 백엔드의 /sample.html로 전달합니다.",
    method: "GET", path: "/proxy/sample.html", expected: 200 }
];

const byId = (id) => document.getElementById(id);
const results = new Map();
let selected = cases[0];

byId("host").textContent = location.host;
for (const item of cases) {
  const button = document.createElement("button");
  button.textContent = item.title;
  button.dataset.id = item.id;
  button.addEventListener("click", () => selectCase(item));
  byId("cases").append(button);
}

function selectCase(item) {
  selected = item;
  for (const button of byId("cases").children) {
    button.setAttribute("aria-pressed", String(button.dataset.id === item.id));
  }
  byId("case-title").textContent = item.title;
  byId("description").textContent = item.description;
  byId("method").textContent = item.method;
  byId("path").textContent = item.path;
  byId("expected").textContent = "예상 상태 " + item.expected;
  byId("message-field").hidden = item.id !== "echo";
  showResult();
}

async function sendRequest(item, path, body) {
  const start = performance.now();
  const options = { method: item.method, cache: "no-store" };
  if (body !== undefined) {
    options.headers = { "Content-Type": "application/json" };
    options.body = body;
  }
  const response = await fetch(path, options);
  const bytes = await response.arrayBuffer();
  return {
    path, text: new TextDecoder().decode(bytes), byteLength: bytes.byteLength,
    status: response.status, reason: response.statusText,
    elapsed: Math.round(performance.now() - start),
    headers: Array.from(response.headers, ([name, value]) => name + ": " + value).join("\n")
  };
}

function element(tag, text) {
  const node = document.createElement(tag);
  node.textContent = text;
  return node;
}

function showResult() {
  const result = results.get(selected.id);
  byId("run").disabled = Boolean(result?.pending);
  byId("responses").replaceChildren();
  byId("status").textContent = result?.message ?? "요청을 보내면 여기에 응답이 표시됩니다.";
  byId("status").dataset.outcome = result?.outcome ?? "";
  for (const reply of result?.replies ?? []) {
    const card = document.createElement("article");
    const heading = element("h4", reply.status + " " + reply.reason + " · " + reply.elapsed + " ms");
    const label = element("h5", "응답 본문 원문 · " + reply.byteLength + " 바이트");
    // HTML도 textContent로 표시해 서버가 보낸 태그와 줄바꿈을 보존한다.
    const body = element("pre", reply.text);
    body.className = "body";
    const headers = document.createElement("details");
    headers.append(element("summary", "응답 헤더"), element("pre", reply.headers));
    if (selected.id === "parallel") card.append(element("code", reply.path));
    card.append(heading, headers, label, body);
    byId("responses").append(card);
  }
}

document.querySelector("form").addEventListener("submit", async (event) => {
  event.preventDefault();
  const item = selected;
  if (results.get(item.id)?.pending) return;
  results.set(item.id, { pending: true, message: "응답을 기다리는 중…" });
  showResult();
  const body = item.id === "echo" ? JSON.stringify({ message: byId("message").value }) : undefined;
  const paths = item.id === "parallel"
    ? [1, 2, 3].map((n) => item.path + "?request=" + n) : [item.path];
  try {
    const replies = await Promise.all(paths.map((path) => sendRequest(item, path, body)));
    const passed = replies.every((reply) => reply.status === item.expected
      && (body === undefined || reply.text === body));
    results.set(item.id, { replies, outcome: passed ? "pass" : "fail",
      message: passed ? "예상한 응답을 받았습니다." : "예상과 다른 응답을 받았습니다." });
  } catch (error) {
    results.set(item.id, { outcome: "fail", message: "요청 실패 · " + error.message });
  }
  // 다른 항목으로 이동했어도 결과는 요청을 시작한 항목에 저장한다.
  if (selected === item) showResult();
});

selectCase(selected);
