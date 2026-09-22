// The notes resource from examples/web_server/main.c, read and written from the page it serves.
//
// A read is a QUERY, not a GET: a request here is a document, and fetch will send any method token
// the protocol allows. The bundle's policy is `connect-src 'self'`, so these two URLs are the only
// ones this file can reach.

const list = document.getElementById("notes");
const status = document.getElementById("status");
const compose = document.getElementById("compose");
const text = document.getElementById("text");

// textContent, never innerHTML: a note is whatever somebody typed, and it is text.
function render(notes) {
  list.replaceChildren();

  for (const note of notes) {
    const item = document.createElement("li");
    item.textContent = note.text;
    list.append(item);
  }

  status.textContent = `${notes.length} note${notes.length === 1 ? "" : "s"}`;
}

async function refresh() {
  try {
    const answer = await fetch("/api/notes", {
      method: "QUERY",
      headers: { "content-type": "application/json" },
      body: "{}",
    });

    if (!answer.ok) throw new Error(`the server answered ${answer.status}`);

    render((await answer.json()).notes ?? []);
  } catch (failure) {
    status.textContent = String(failure);
  }
}

compose.addEventListener("submit", async (event) => {
  event.preventDefault();

  if (text.value.trim() === "") return;

  await fetch("/api/notes", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ text: text.value }),
  });

  text.value = "";
  await refresh();
});

refresh();
