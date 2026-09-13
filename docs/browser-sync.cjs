const path = require("node:path");
const output = path.resolve(__dirname, "../docs_output");

module.exports = {
  server: { baseDir: output },
  files: [path.join(output, "**/*.html"), path.join(output, "**/*.css")],
  // Poll metadata because native watch events can vary on virtual filesystems.
  watchOptions: { ignoreInitial: true, usePolling: true, interval: 25 },
  // Keep a small batching window instead of BrowserSync's 500 ms default.
  reloadDebounce: 25,
  listen: "127.0.0.1",
  port: 8000,
  open: false,
  ui: false,
  notify: false,
  ghostMode: false,
};
