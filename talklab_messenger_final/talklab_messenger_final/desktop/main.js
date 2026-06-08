const { app, BrowserWindow, Menu } = require("electron");

const APP_URL = process.env.APP_URL || "http://localhost:8080";

function createWindow() {
  const win = new BrowserWindow({
    width: 430,
    height: 740,
    resizable: true,
    backgroundColor: "#ffdf32",
    title: "TalkLab",
    webPreferences: {
      nodeIntegration: false,
      contextIsolation: true,
      sandbox: true,
    },
  });

  Menu.setApplicationMenu(null);
  win.loadURL(APP_URL);
}

app.whenReady().then(createWindow);

app.on("window-all-closed", () => {
  if (process.platform !== "darwin") app.quit();
});

app.on("activate", () => {
  if (BrowserWindow.getAllWindows().length === 0) createWindow();
});
