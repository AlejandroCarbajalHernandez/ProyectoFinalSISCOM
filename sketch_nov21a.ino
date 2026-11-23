#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <VL53L0X.h>

// --- CONFIGURACIÓN DE RED (HOTSPOT IPHONE 2.4 GHz) ---
const char* ssid = "AlexiPhone";
const char* password = "N4cH0v1d56";

// --- DEFINICIÓN DE PINES ---
#define SDA_PIN    21
#define SCL_PIN    22
#define BUZZER_PIN 25

VL53L0X sensor;
WebServer server(80);

// --- VARIABLES DE TELEMETRÍA Y FILTRADO ---
int distanciaCm = 999;
int distAnterior = 999;
unsigned long tiempoMuestraVel = 0;
bool acercamientoRapido = false;
int lecturasConsecutivas = 0;

String estadoAlerta = "CAMINO DESPEJADO";
unsigned long ultimoPulsoBuzzer = 0;
bool estadoBuzzer = false;

// --- INTERFAZ WEB ACCESIBLE (VOZ SIN BLOQUEOS + RESPUESTA INMEDIATA) ---
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="es">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0, user-scalable=no">
  <title>Bastón Inteligente</title>
  <style>
    * { box-sizing: border-box; user-select: none; -webkit-tap-highlight-color: transparent; }
    body {
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
      background-color: #000000;
      color: #FFFFFF;
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: space-between;
      min-height: 100vh;
      margin: 0;
      padding: 28px 20px;
      text-align: center;
      cursor: pointer;
    }
    .header { font-size: 1.2rem; color: #93C5FD; font-weight: 700; letter-spacing: 0.5px; }
    .distancia { font-size: 5rem; font-weight: 900; margin: 10px 0; line-height: 1; }
    .estado {
      font-size: 1.5rem;
      font-weight: 800;
      padding: 16px 24px;
      border-radius: 16px;
      width: 100%;
      max-width: 420px;
      background: #065F46;
      border: 3px solid #10B981;
    }
    .precaucion { background: #92400E !important; border-color: #F59E0B !important; }
    .peligro { background: #7F1D1D !important; border-color: #EF4444 !important; }
    .desconectado { background: #374151 !important; border-color: #EF4444 !important; }
    .voz-indicador {
      width: 100%;
      max-width: 420px;
      padding: 22px;
      border-radius: 16px;
      font-size: 1.3rem;
      font-weight: 800;
      background: #1D4ED8;
      border: 3px solid #60A5FA;
    }
    .voz-indicador.activo { background: #047857; border-color: #34D399; }
  </style>
</head>
<body onclick="toggleVoz()">
  <div class="header">ASISTENTE DE MOVILIDAD</div>
  <div class="distancia" id="dist">-- cm</div>
  <div class="estado" id="estado" aria-live="assertive">Iniciando...</div>
  <div class="voz-indicador" id="btnVoz">TOCA O HAZ CLIC PARA ACTIVAR VOZ</div>

  <script>
    let vozActiva = false;
    let ultimoAviso = 0;
    let ultimoNivel = 0; // 0 = despejado, 1 = precaución, 2 = peligro
    let ultimoPaquete = Date.now();
    let avisoDesconexionDado = false;
    window.utteranceActual = null; // Evita que el navegador elimine la voz a mitad de frase

    function toggleVoz() {
      vozActiva = !vozActiva;
      const btn = document.getElementById('btnVoz');
      if (vozActiva) {
        btn.textContent = "VOZ ACTIVA (CLIC PARA PAUSAR)";
        btn.classList.add('activo');
        hablar("Voz activada", true);
      } else {
        btn.textContent = "TOCA O HAZ CLIC PARA ACTIVAR VOZ";
        btn.classList.remove('activo');
        if ('speechSynthesis' in window) window.speechSynthesis.cancel();
      }
    }

    function hablar(texto, interrumpir = false) {
      if (!vozActiva || !('speechSynthesis' in window)) return;

      const lanzarVoz = () => {
        window.speechSynthesis.resume();
        const msg = new SpeechSynthesisUtterance(texto);
        msg.lang = 'es-MX';
        msg.rate = 1.35;
        msg.onend = () => { window.utteranceActual = null; };
        msg.onerror = () => { window.utteranceActual = null; };
        window.utteranceActual = msg;
        window.speechSynthesis.speak(msg);
      };

      if (interrumpir || window.speechSynthesis.speaking) {
        window.speechSynthesis.cancel();
        // Pequeña pausa de 40 ms para evitar que Safari/Chrome cancelen la frase nueva
        setTimeout(lanzarVoz, 40);
      } else {
        lanzarVoz();
      }
    }

    // Watchdog: alerta si se pierde conexión con el ESP32 (> 1.5 s)
    setInterval(() => {
      if (Date.now() - ultimoPaquete > 1500) {
        const estEl = document.getElementById('estado');
        estEl.textContent = "SIN CONEXIÓN CON EL BASTÓN";
        estEl.className = "estado desconectado";
        if (!avisoDesconexionDado) {
          hablar("Sin conexión con el bastón", true);
          avisoDesconexionDado = true;
        }
      }
    }, 500);

    // Consulta rápida de telemetría cada 150 ms
    setInterval(() => {
      fetch('/data')
        .then(res => res.json())
        .then(data => {
          ultimoPaquete = Date.now();
          avisoDesconexionDado = false;

          const distEl = document.getElementById('dist');
          const estEl = document.getElementById('estado');

          distEl.textContent = data.distancia >= 200 ? "> 200 cm" : data.distancia + " cm";
          estEl.textContent = data.estado;
          estEl.className = "estado";

          const ahora = Date.now();
          let nivelActual = 0;

          if (data.urgente || data.distancia <= 40) {
            nivelActual = 2;
            estEl.classList.add('peligro');
            // Habla de inmediato si subió de nivel o si ya pasaron 1.6 s
            if (nivelActual > ultimoNivel || (ahora - ultimoAviso > 1600)) {
              hablar("¡Alto! " + data.distancia, true);
              ultimoAviso = ahora;
            }
          } else if (data.distancia <= 90) {
            nivelActual = 1;
            estEl.classList.add('precaucion');
            // Habla de inmediato al detectar el objeto por primera vez o cada 2.5 s
            if (nivelActual > ultimoNivel || (ahora - ultimoAviso > 2500)) {
              hablar("Cuidado, " + data.distancia, nivelActual > ultimoNivel);
              ultimoAviso = ahora;
            }
          } else {
            nivelActual = 0;
          }

          ultimoNivel = nivelActual;
        })
        .catch(() => {});
    }, 150);
  </script>
</body>
</html>
)rawliteral";

// --- MANEJADORES HTTP ---
void handleRoot() {
  server.send_P(200, "text/html", index_html);
}

void handleData() {
  bool esUrgente = (distanciaCm <= 40) || acercamientoRapido;
  String json = "{\"distancia\":" + String(distanciaCm) +
                ",\"estado\":\"" + estadoAlerta + "\"" +
                ",\"urgente\":" + (esUrgente ? "true" : "false") + "}";
  server.send(200, "application/json", json);
}

// --- CONTROL ACÚSTICO EN BANDA DE RESONANCIA ---
void actualizarBuzzer(int cm, bool urgente) {
  if (cm > 90 && !urgente) {
    noTone(BUZZER_PIN);
    estadoBuzzer = false;
    return;
  }

  unsigned long intervalo = (cm <= 40 || urgente) ? 110 : map(cm, 41, 90, 200, 500);
  int frecuencia = (cm <= 40 || urgente) ? 2700 : 2500;

  if (millis() - ultimoPulsoBuzzer >= intervalo) {
    ultimoPulsoBuzzer = millis();
    estadoBuzzer = !estadoBuzzer;
    if (estadoBuzzer) {
      tone(BUZZER_PIN, frecuencia);
    } else {
      noTone(BUZZER_PIN);
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(BUZZER_PIN, OUTPUT);
  noTone(BUZZER_PIN);

  tone(BUZZER_PIN, 2600, 200);
  delay(250);

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);
  delay(100);

  sensor.setBus(&Wire);
  sensor.setTimeout(500);

  if (!sensor.init()) {
    Serial.println(F("ERROR: Sensor VL53L0X no detectado"));
    while (1) {
      delay(100);
    }
  }

  // Lectura rápida (20 ms) para reducir latencia en el primer movimiento
  sensor.setMeasurementTimingBudget(20000);
  sensor.startContinuous();
  Serial.println(F("Sensor VL53L0X inicializado correctamente."));

  // Espera hasta 30 segundos (60 x 500 ms) al Hotspot del iPhone
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print(F("Conectando a Wi-Fi"));

  int intentos = 0;
  while (WiFi.status() != WL_CONNECTED && intentos < 60) {
    delay(500);
    Serial.print(".");
    intentos++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(F("\nWi-Fi conectado!"));
    Serial.print(F("Abre esta URL: http://"));
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(F("\nHotspot no disponible. Iniciando red propia..."));
    WiFi.mode(WIFI_AP);
    WiFi.softAP("Baston_Inteligente", "12345678");
    Serial.print(F("Conéctate a 'Baston_Inteligente' y abre: http://"));
    Serial.println(WiFi.softAPIP());
  }

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.begin();
}

void loop() {
  server.handleClient();

  uint16_t mm = sensor.readRangeContinuousMillimeters();

  if (!sensor.timeoutOccurred() && mm < 2000) {
    distanciaCm = mm / 10;
  } else {
    distanciaCm = 999;
  }

  // Cálculo de velocidad de acercamiento cada 250 ms
  if (millis() - tiempoMuestraVel >= 250) {
    int delta = distAnterior - distanciaCm;
    acercamientoRapido = (delta >= 30 && distanciaCm <= 120);
    distAnterior = distanciaCm;
    tiempoMuestraVel = millis();
  }

  // Filtro ágil: 2 lecturas seguidas (~40 ms) para reacción inmediata sin falsos positivos
  if (distanciaCm <= 90 || acercamientoRapido) {
    lecturasConsecutivas++;
  } else {
    lecturasConsecutivas = 0;
  }

  if (lecturasConsecutivas >= 2) {
    if (acercamientoRapido) {
      estadoAlerta = "¡ALTO! OBJETO RÁPIDO";
    } else if (distanciaCm <= 40) {
      estadoAlerta = "¡ALTO! OBSTÁCULO CERCA";
    } else {
      estadoAlerta = "CUIDADO: OBJETO PRÓXIMO";
    }
    actualizarBuzzer(distanciaCm, acercamientoRapido);
  } else {
    estadoAlerta = "CAMINO DESPEJADO";
    noTone(BUZZER_PIN);
    estadoBuzzer = false;
  }

  delay(10);
}