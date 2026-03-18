const canvas = document.getElementById("game");
const ctx = canvas.getContext("2d");
const overlay = document.getElementById("overlay");
const startButton = document.getElementById("startButton");
const scoreEl = document.getElementById("score");
const bestEl = document.getElementById("best");
const speedEl = document.getElementById("speed");

const STORAGE_KEY = "subway-runner-best-score";
const lanes = [canvas.width * 0.28, canvas.width * 0.5, canvas.width * 0.72];
const horizonY = 118;
const groundY = canvas.height - 90;

const state = {
  running: false,
  lastTime: 0,
  distance: 0,
  score: 0,
  best: Number(localStorage.getItem(STORAGE_KEY) || 0),
  speed: 420,
  spawnTimer: 0,
  coinTimer: 0,
  difficultyTimer: 0,
  message: "点击开始游戏",
  player: null,
  obstacles: [],
  coins: [],
  particles: [],
};

bestEl.textContent = state.best;

function resetGame() {
  state.running = true;
  state.lastTime = 0;
  state.distance = 0;
  state.score = 0;
  state.speed = 420;
  state.spawnTimer = 0;
  state.coinTimer = 0;
  state.difficultyTimer = 0;
  state.message = "";
  state.obstacles = [];
  state.coins = [];
  state.particles = [];
  state.player = {
    lane: 1,
    x: lanes[1],
    y: groundY,
    width: 54,
    height: 88,
    vy: 0,
    jumpForce: 900,
    gravity: 2300,
    jumpCount: 0,
    slideTimer: 0,
    invincibleTimer: 0,
  };
  overlay.classList.add("hidden");
  updateHud();
}

function updateHud() {
  scoreEl.textContent = Math.floor(state.score);
  bestEl.textContent = state.best;
  speedEl.textContent = (state.speed / 420).toFixed(1);
}

function moveLane(direction) {
  if (!state.running || !state.player) return;
  state.player.lane = Math.max(0, Math.min(2, state.player.lane + direction));
}

function jump() {
  const player = state.player;
  if (!state.running || !player) return;
  if (player.jumpCount < 2) {
    player.vy = -player.jumpForce;
    player.jumpCount += 1;
    player.slideTimer = 0;
  }
}

function slide() {
  const player = state.player;
  if (!state.running || !player) return;
  if (player.y >= groundY - 0.5) {
    player.slideTimer = 0.65;
  }
}

function spawnObstacle() {
  const lane = Math.floor(Math.random() * 3);
  const roll = Math.random();
  let type = "barrier";
  let width = 86;
  let height = 80;
  let requires = "jump";
  if (roll > 0.68) {
    type = "train";
    width = 120;
    height = 132;
    requires = "lane";
  } else if (roll > 0.38) {
    type = "duck";
    width = 95;
    height = 62;
    requires = "slide";
  }
  state.obstacles.push({
    type,
    lane,
    x: lanes[lane],
    y: groundY,
    width,
    height,
    requires,
    z: 0,
  });
}

function spawnCoins() {
  const lane = Math.floor(Math.random() * 3);
  const patternSize = 3 + Math.floor(Math.random() * 4);
  const airborne = Math.random() > 0.5;
  for (let i = 0; i < patternSize; i += 1) {
    state.coins.push({
      lane,
      x: lanes[lane],
      y: airborne ? groundY - 72 - i * 18 : groundY - 16,
      z: i * 70,
      radius: 14,
      collected: false,
    });
  }
}

function addParticles(x, y, color) {
  for (let i = 0; i < 8; i += 1) {
    state.particles.push({
      x,
      y,
      vx: (Math.random() - 0.5) * 180,
      vy: -Math.random() * 180,
      life: 0.45 + Math.random() * 0.2,
      color,
    });
  }
}

function collideRect(player, obstacle) {
  const playerHeight = player.slideTimer > 0 ? player.height * 0.55 : player.height;
  const playerTop = player.y - playerHeight;
  const playerBottom = player.y;
  const playerLeft = player.x - player.width * 0.5;
  const playerRight = player.x + player.width * 0.5;

  const obstacleTop = obstacle.y - obstacle.height;
  const obstacleBottom = obstacle.y;
  const obstacleLeft = obstacle.x - obstacle.width * 0.5;
  const obstacleRight = obstacle.x + obstacle.width * 0.5;

  return !(playerRight < obstacleLeft || playerLeft > obstacleRight || playerBottom < obstacleTop || playerTop > obstacleBottom);
}

function endGame() {
  state.running = false;
  state.message = `游戏结束，得分 ${Math.floor(state.score)}`;
  state.best = Math.max(state.best, Math.floor(state.score));
  localStorage.setItem(STORAGE_KEY, String(state.best));
  updateHud();
  overlay.innerHTML = `
    <h1>${state.message}</h1>
    <p>按下方按钮立即再来一局，或者按空格开始。</p>
    <button id="startButton" type="button">重新开始</button>
  `;
  overlay.classList.remove("hidden");
  document.getElementById("startButton").addEventListener("click", startGame, { once: true });
}

function update(dt) {
  if (!state.running || !state.player) return;
  const player = state.player;

  state.distance += state.speed * dt;
  state.score += dt * 28 + state.speed * 0.01;
  state.spawnTimer += dt;
  state.coinTimer += dt;
  state.difficultyTimer += dt;

  if (state.difficultyTimer >= 3.2) {
    state.speed = Math.min(900, state.speed + 28);
    state.difficultyTimer = 0;
  }

  if (state.spawnTimer >= Math.max(0.48, 1.12 - (state.speed - 420) / 800)) {
    spawnObstacle();
    state.spawnTimer = 0;
  }

  if (state.coinTimer >= 1.3) {
    spawnCoins();
    state.coinTimer = 0;
  }

  player.x += (lanes[player.lane] - player.x) * Math.min(1, dt * 12);
  player.vy += player.gravity * dt;
  player.y += player.vy * dt;

  if (player.y > groundY) {
    player.y = groundY;
    player.vy = 0;
    player.jumpCount = 0;
  }

  if (player.slideTimer > 0) {
    player.slideTimer -= dt;
  }
  if (player.invincibleTimer > 0) {
    player.invincibleTimer -= dt;
  }

  state.obstacles.forEach((obstacle) => {
    obstacle.z += state.speed * dt;
  });
  state.obstacles = state.obstacles.filter((obstacle) => obstacle.z < 1100);

  state.coins.forEach((coin) => {
    coin.z += state.speed * dt;
  });
  state.coins = state.coins.filter((coin) => coin.z < 1000 && !coin.collected);

  state.particles.forEach((particle) => {
    particle.x += particle.vx * dt;
    particle.y += particle.vy * dt;
    particle.vy += 360 * dt;
    particle.life -= dt;
  });
  state.particles = state.particles.filter((particle) => particle.life > 0);

  for (const obstacle of state.obstacles) {
    if (Math.abs(obstacle.z - 920) < 110 && obstacle.lane === player.lane && collideRect(player, obstacle)) {
      const safeByJump = obstacle.requires === "jump" && player.y < groundY - 78;
      const safeBySlide = obstacle.requires === "slide" && player.slideTimer > 0;
      const safeByLane = obstacle.requires === "lane" && obstacle.lane !== player.lane;
      if (!(safeByJump || safeBySlide || safeByLane)) {
        endGame();
        return;
      }
    }
  }

  for (const coin of state.coins) {
    const coinAtPlayer = Math.abs(coin.z - 920) < 70 && coin.lane === player.lane && Math.abs((player.y - 42) - coin.y) < 62;
    if (coinAtPlayer) {
      coin.collected = true;
      state.score += 25;
      addParticles(player.x, player.y - 60, "#f6e05e");
    }
  }

  state.best = Math.max(state.best, Math.floor(state.score));
  localStorage.setItem(STORAGE_KEY, String(state.best));
  updateHud();
}

function project(z) {
  return Math.max(0.16, 1 - z / 1200);
}

function drawBackground() {
  const sky = ctx.createLinearGradient(0, 0, 0, canvas.height);
  sky.addColorStop(0, "#102542");
  sky.addColorStop(0.55, "#163a63");
  sky.addColorStop(1, "#08111f");
  ctx.fillStyle = sky;
  ctx.fillRect(0, 0, canvas.width, canvas.height);

  ctx.fillStyle = "rgba(255,255,255,0.06)";
  for (let i = 0; i < 18; i += 1) {
    const w = 40 + (i % 4) * 16;
    const h = 30 + (i % 5) * 18;
    const x = (i * 71 + (state.distance * 0.08) % 140) % (canvas.width + 120) - 60;
    const y = 78 + (i % 3) * 20;
    ctx.fillRect(x, y, w, h);
  }

  ctx.fillStyle = "#0b2037";
  ctx.beginPath();
  ctx.moveTo(0, horizonY + 24);
  ctx.lineTo(canvas.width * 0.18, horizonY - 16);
  ctx.lineTo(canvas.width * 0.36, horizonY + 28);
  ctx.lineTo(canvas.width * 0.58, horizonY - 22);
  ctx.lineTo(canvas.width * 0.8, horizonY + 18);
  ctx.lineTo(canvas.width, horizonY - 8);
  ctx.lineTo(canvas.width, canvas.height);
  ctx.lineTo(0, canvas.height);
  ctx.closePath();
  ctx.fill();
}

function drawTrack() {
  ctx.fillStyle = "#17283b";
  ctx.beginPath();
  ctx.moveTo(canvas.width * 0.18, canvas.height);
  ctx.lineTo(canvas.width * 0.38, horizonY);
  ctx.lineTo(canvas.width * 0.62, horizonY);
  ctx.lineTo(canvas.width * 0.82, canvas.height);
  ctx.closePath();
  ctx.fill();

  for (let i = 0; i < 36; i += 1) {
    const z = (state.distance * 1.2 + i * 90) % 1200;
    const scale = project(z);
    const y = horizonY + (groundY - horizonY) * scale;
    const left = canvas.width * 0.5 - 205 * scale;
    const right = canvas.width * 0.5 + 205 * scale;

    ctx.strokeStyle = i % 2 === 0 ? "rgba(255,255,255,0.22)" : "rgba(255,255,255,0.08)";
    ctx.lineWidth = Math.max(1, 6 * scale);
    ctx.beginPath();
    ctx.moveTo(left, y);
    ctx.lineTo(right, y);
    ctx.stroke();
  }

  [0.33, 0.5, 0.67].forEach((fraction, index) => {
    ctx.strokeStyle = index === 1 ? "rgba(79,209,197,0.35)" : "rgba(255,255,255,0.14)";
    ctx.lineWidth = 2;
    ctx.beginPath();
    ctx.moveTo(canvas.width * fraction, canvas.height);
    ctx.lineTo(canvas.width * 0.5 + (fraction - 0.5) * 120, horizonY);
    ctx.stroke();
  });
}

function drawObstacle(obstacle) {
  const scale = project(obstacle.z);
  const screenX = canvas.width * 0.5 + (obstacle.x - canvas.width * 0.5) * scale;
  const screenY = horizonY + (groundY - horizonY) * scale;
  const width = obstacle.width * scale;
  const height = obstacle.height * scale;

  if (obstacle.type === "train") {
    ctx.fillStyle = "#e53e3e";
    ctx.fillRect(screenX - width / 2, screenY - height, width, height);
    ctx.fillStyle = "#fed7d7";
    ctx.fillRect(screenX - width * 0.28, screenY - height * 0.78, width * 0.18, height * 0.2);
    ctx.fillRect(screenX + width * 0.1, screenY - height * 0.78, width * 0.18, height * 0.2);
  } else {
    ctx.fillStyle = obstacle.type === "duck" ? "#d69e2e" : "#718096";
    ctx.fillRect(screenX - width / 2, screenY - height, width, height);
    ctx.fillStyle = "rgba(255,255,255,0.18)";
    ctx.fillRect(screenX - width / 2, screenY - height, width, height * 0.2);
  }
}

function drawCoin(coin) {
  const scale = project(coin.z);
  const screenX = canvas.width * 0.5 + (coin.x - canvas.width * 0.5) * scale;
  const roadY = horizonY + (groundY - horizonY) * scale;
  const screenY = roadY - (groundY - coin.y) * scale;
  const radius = coin.radius * scale;

  ctx.fillStyle = "#f6e05e";
  ctx.beginPath();
  ctx.arc(screenX, screenY, radius, 0, Math.PI * 2);
  ctx.fill();
  ctx.strokeStyle = "rgba(255,255,255,0.45)";
  ctx.lineWidth = Math.max(1, radius * 0.18);
  ctx.stroke();
}

function drawPlayer() {
  const player = state.player;
  if (!player) return;
  const height = player.slideTimer > 0 ? player.height * 0.56 : player.height;
  const width = player.slideTimer > 0 ? player.width * 1.18 : player.width;
  const top = player.y - height;

  ctx.save();
  if (player.invincibleTimer > 0) {
    ctx.globalAlpha = 0.65;
  }

  ctx.fillStyle = "#4fd1c5";
  ctx.fillRect(player.x - width / 2, top + 20, width, height - 20);
  ctx.fillStyle = "#fbd38d";
  ctx.beginPath();
  ctx.arc(player.x, top + 12, 18, 0, Math.PI * 2);
  ctx.fill();
  ctx.fillStyle = "#f6ad55";
  ctx.fillRect(player.x - width / 2, player.y - 14, width * 0.32, 14);
  ctx.fillRect(player.x + width * 0.18, player.y - 14, width * 0.32, 14);
  ctx.restore();

  ctx.strokeStyle = "rgba(255,255,255,0.22)";
  ctx.lineWidth = 3;
  ctx.beginPath();
  ctx.arc(player.x, player.y + 8, 18, 0, Math.PI, false);
  ctx.stroke();
}

function drawParticles() {
  state.particles.forEach((particle) => {
    ctx.globalAlpha = Math.max(0, particle.life * 1.8);
    ctx.fillStyle = particle.color;
    ctx.beginPath();
    ctx.arc(particle.x, particle.y, 4, 0, Math.PI * 2);
    ctx.fill();
  });
  ctx.globalAlpha = 1;
}

function render() {
  drawBackground();
  drawTrack();

  [...state.coins]
    .sort((a, b) => b.z - a.z)
    .forEach(drawCoin);

  [...state.obstacles]
    .sort((a, b) => b.z - a.z)
    .forEach(drawObstacle);

  drawPlayer();
  drawParticles();

  if (!state.running && state.message) {
    ctx.fillStyle = "rgba(255,255,255,0.8)";
    ctx.font = "600 24px Segoe UI";
    ctx.fillText(state.message, 24, 38);
  }
}

function gameLoop(timestamp) {
  if (!state.lastTime) {
    state.lastTime = timestamp;
  }
  const dt = Math.min(0.032, (timestamp - state.lastTime) / 1000);
  state.lastTime = timestamp;
  update(dt);
  render();
  requestAnimationFrame(gameLoop);
}

function startGame() {
  resetGame();
}

window.addEventListener("keydown", (event) => {
  if (["ArrowLeft", "a", "A"].includes(event.key)) {
    moveLane(-1);
  }
  if (["ArrowRight", "d", "D"].includes(event.key)) {
    moveLane(1);
  }
  if (["ArrowUp", "w", "W", " "].includes(event.key)) {
    if (!state.running) {
      startGame();
    } else {
      jump();
    }
    event.preventDefault();
  }
  if (["ArrowDown", "s", "S"].includes(event.key)) {
    slide();
    event.preventDefault();
  }
});

let touchStartX = 0;
let touchStartY = 0;
canvas.addEventListener("touchstart", (event) => {
  const touch = event.changedTouches[0];
  touchStartX = touch.clientX;
  touchStartY = touch.clientY;
}, { passive: true });

canvas.addEventListener("touchend", (event) => {
  const touch = event.changedTouches[0];
  const dx = touch.clientX - touchStartX;
  const dy = touch.clientY - touchStartY;

  if (!state.running) {
    startGame();
    return;
  }

  if (Math.abs(dx) > Math.abs(dy)) {
    moveLane(dx > 0 ? 1 : -1);
  } else if (dy < -20) {
    jump();
  } else if (dy > 20) {
    slide();
  }
}, { passive: true });

startButton.addEventListener("click", startGame);
render();
requestAnimationFrame(gameLoop);
