const express = require("express");
const http = require("http");
const { Server } = require("socket.io");
const path = require("path");

const app = express();
const server = http.createServer(app);
const io = new Server(server);

const PORT = process.env.PORT || 3000;
const MAX_EVENTS = 50; // keep last 50 events in memory

// In-memory event store
const events = [];

// Serve static dashboard
app.use(express.static(path.join(__dirname, "public")));

// Health check
app.get("/api/health", (req, res) => {
  res.json({ status: "ok", events: events.length, uptime: process.uptime() });
});

// Get all stored events (without image data to keep it light)
app.get("/api/events", (req, res) => {
  res.json(events);
});

// Get a specific event image
app.get("/api/events/:id/image", (req, res) => {
  const event = events.find((e) => e.id === req.params.id);
  if (!event || !event.imageBase64) {
    return res.status(404).send("Not found");
  }
  const imgBuffer = Buffer.from(event.imageBase64, "base64");
  res.set("Content-Type", "image/jpeg");
  res.send(imgBuffer);
});

// Receive detection event from ESP32
// POST /api/event?score=XXX — body is raw JPEG
app.post("/api/event", (req, res) => {
  const score = parseInt(req.query.score) || 0;
  const chunks = [];

  req.on("data", (chunk) => chunks.push(chunk));
  req.on("end", () => {
    const imageBuffer = Buffer.concat(chunks);
    const imageBase64 = imageBuffer.toString("base64");

    const event = {
      id: Date.now().toString(36) + Math.random().toString(36).slice(2, 6),
      timestamp: new Date().toISOString(),
      score: score,
      imageBase64: imageBase64,
      imageSize: imageBuffer.length,
    };

    events.unshift(event); // newest first
    if (events.length > MAX_EVENTS) {
      events.pop(); // drop oldest
    }

    // Push to all connected dashboard clients
    io.emit("detection", {
      id: event.id,
      timestamp: event.timestamp,
      score: event.score,
      imageSize: event.imageSize,
    });

    console.log(
      `[${event.timestamp}] Person detected — score: ${score}, image: ${imageBuffer.length} bytes`
    );
    res.json({ status: "ok", id: event.id });
  });
});

// Socket.io connection
io.on("connection", (socket) => {
  console.log("Dashboard client connected");

  // Send existing events on connect (without full image data)
  socket.emit(
    "history",
    events.map((e) => ({
      id: e.id,
      timestamp: e.timestamp,
      score: e.score,
      imageSize: e.imageSize,
    }))
  );

  socket.on("disconnect", () => {
    console.log("Dashboard client disconnected");
  });
});

server.listen(PORT, () => {
  console.log(`\n  ESP32-CAM Dashboard running on port ${PORT}`);
  console.log(`  Dashboard:  http://localhost:${PORT}`);
  console.log(`  API health: http://localhost:${PORT}/api/health\n`);
});
