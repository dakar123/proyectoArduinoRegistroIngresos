/*
 * =====================================================
 *   SISTEMA DE REGISTRO DE INGRESOS Y SALIDAS
 *   (Terminal Dual: Entrada y Salida Independientes + SD)
 *   Arduino Mega + 2x OLED + 2x Keypad + MAX7219 + 2x PIR + SD + RTC
 *   Base de datos local en EEPROM y respaldo en SD
 *   Historial Circular en EEPROM
 *   ADMINISTRADOR PRINCIPAL OCULTO: ID "A000"
 * =====================================================
 */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Keypad.h>
#include <MD_Parola.h>
#include <MD_MAX72xx.h>
#include <SPI.h>
#include <EEPROM.h>
#include <SD.h>
#include <RTClib.h>

// ==================== CONFIGURACION RTC ====================
RTC_DS1307 rtc;
bool rtcActivo = false;

// ==================== CONFIGURACION OLED ====================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

// OLED Entrada: Address 0x3D
Adafruit_SSD1306 oledEntrada(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
// OLED Salida: Address 0x3C
Adafruit_SSD1306 oledSalida(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ==================== CONFIGURACION MATRIZ LED ====================
#define HARDWARE_TYPE MD_MAX72XX::PAROLA_HW
#define MAX_DEVICES 8
#define CS_PIN 8 
#define DATA_PIN 51
#define CLK_PIN 52
MD_Parola panel = MD_Parola(HARDWARE_TYPE, DATA_PIN, CLK_PIN, CS_PIN, MAX_DEVICES);

// ==================== CONFIGURACION SD CARD ====================
#define SD_CS_PIN 53
bool sdActiva = false;

// ==================== CONFIGURACION TECLADOS ====================
const byte ROWS = 4;
const byte COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};

// Teclado Entrada
byte rowPinsEntrada[ROWS] = {22, 24, 26, 28};
byte colPinsEntrada[COLS] = {30, 32, 34, 36};
Keypad keypadEntrada = Keypad(makeKeymap(keys), rowPinsEntrada, colPinsEntrada, ROWS, COLS);

// Teclado Salida
byte rowPinsSalida[ROWS] = {23, 25, 27, 29};
byte colPinsSalida[COLS] = {31, 33, 35, 37};
Keypad keypadSalida = Keypad(makeKeymap(keys), rowPinsSalida, colPinsSalida, ROWS, COLS);

// ==================== PINES Y HARDWARE ====================
#define PIR_ENTRADA_PIN 2
#define PIR_SALIDA_PIN 7
#define BUZZER_PIN 3
#define LED_DENIED 4
#define LED_GRANTED 5
#define LED_ALARM 6

const int doorLeds[15] = {38,39,40,41,42,43,44,45,46,47,48,49,A0,A1,A2};

// ==================== VARIABLES T9 ====================
const char* t9map[] = {
  "0 ",     // 0
  "1",      // 1
  "2abc",   // 2
  "3def",   // 3
  "4ghi",   // 4
  "5jkl",   // 5
  "6mno",   // 6
  "7pqrs",  // 7
  "8tuv",   // 8
  "9wxyz"   // 9
};
const unsigned long T9_TIMEOUT = 800;
const unsigned long INACTIVITY_TIMEOUT = 10000;
const unsigned long SD_RETRY_INTERVAL = 30000;
unsigned long lastSDFatalTime = 0;

// ==================== BASE DE DATOS EEPROM ====================
#define MAX_USERS 20
#define NAME_LEN 13      
#define PASS_LEN 9       
// Usamos 0xAE como Magic Number para asegurar reseteo limpio en este update si es necesario
#define EEPROM_MAGIC 0xAE 

struct User {
  uint16_t id;            
  char name[NAME_LEN];
  char password[PASS_LEN];
  uint8_t role;           // 1=ADMIN, 0=EMPLEADO
  uint8_t loggedIn;       
  uint8_t active;         
};

#define USER_SIZE sizeof(User)
#define ADDR_MAGIC 0
#define ADDR_COUNT 1
#define ADDR_USERS 2

// ==================== HISTORIAL EN EEPROM ====================
#define MAX_HISTORY 30
struct AccessLog {
  uint16_t id;
  uint16_t year;
  uint8_t month;
  uint8_t day;
  uint8_t hour;
  uint8_t minute;
  uint8_t action; // 1=IN, 0=OUT
  uint8_t padding; // For 10 bytes alignment
};

#define ADDR_HISTORY_HEAD 600
#define ADDR_HISTORY_DATA 602

// Panel LED
char panelMsg[60] = "ENTRADA ->   <- SALIDA";
bool panelNeedsUpdate = true;

// ==================== LOGICA DE TERMINAL INDEPENDIENTE ====================
enum Estado {
  REPOSO,
  LOGIN_ID,
  LOGIN_PASS,
  MENU_ADMIN,
  ADMIN_ADD_NAME,
  ADMIN_ADD_PASS,
  ADMIN_ADD_ROLE,
  ADMIN_DEL,
  ADMIN_LIST,
  ADMIN_HISTORY
};

enum TipoTerminal {
  TERMINAL_ENTRADA = 1,
  TERMINAL_SALIDA = 2
};

struct Terminal {
  TipoTerminal tipo;
  Adafruit_SSD1306* display;
  Keypad* keypad;
  int pirPin;
  
  Estado estadoActual;
  String inputBuffer;
  int intentos;
  int loginUserId;
  int adminUserIdx;
  
  bool modoAlfanumerico;
  char lastT9Key;
  int t9PressCount;
  unsigned long lastT9Time;
  unsigned long lastActivityTime;
  
  int listaOffset;
  
  // Buffers temporales para admin
  char tempName[NAME_LEN];
  char tempPass[PASS_LEN];
};

Terminal termEntrada;
Terminal termSalida;

// ==================== PROTOTIPOS ====================
void respaldarEnSD(const char* accion, const User& u);
void registrarHistorial(uint16_t id, uint8_t action);
bool obtenerHistorial(int offset, AccessLog &log);
void irAReposo(Terminal* t);
void oledClear(Terminal* t);
void oledTitulo(Terminal* t, const char* txt);
void oledTexto(Terminal* t, const char* txt, int x, int y, int size);
void oledMostrar(Terminal* t);

// ==================== FUNCIONES EEPROM, SD Y RTC ====================

void respaldarEnSD(const char* accion, const User& u) {
  if (!sdActiva) return;
  
  File logFile = SD.open("users.txt", FILE_WRITE);
  if (logFile) {
    if (rtcActivo) {
        DateTime now = rtc.now();
        logFile.print(now.year()); logFile.print('/');
        logFile.print(now.month()); logFile.print('/');
        logFile.print(now.day()); logFile.print(' ');
        logFile.print(now.hour()); logFile.print(':');
        logFile.print(now.minute()); logFile.print(',');
    }
    
    logFile.print(accion);
    logFile.print(",");
    logFile.print(u.id);
    logFile.print(",");
    logFile.print(u.name);
    logFile.print(",");
    logFile.print(u.role == 1 ? "ADMIN" : "EMP");
    logFile.print(",Logged:");
    logFile.println(u.loggedIn);
    logFile.close();
  } else {
    Serial.println("Error abriendo users.txt - posible desconexion de SD");
    sdActiva = false; // Disable to prevent massive lags
    lastSDFatalTime = millis();
  }
}

void registrarHistorial(uint16_t id, uint8_t action) {
  DateTime now;
  if (rtcActivo) {
    now = rtc.now();
  } else {
    // Valores nulos si no hay RTC
    now = DateTime(0, 0, 0, 0, 0, 0);
  }
  
  uint8_t head = EEPROM.read(ADDR_HISTORY_HEAD);
  if (head >= MAX_HISTORY) head = 0; 
  
  AccessLog logEntry;
  logEntry.id = id;
  logEntry.year = now.year();
  logEntry.month = now.month();
  logEntry.day = now.day();
  logEntry.hour = now.hour();
  logEntry.minute = now.minute();
  logEntry.action = action;
  
  int addr = ADDR_HISTORY_DATA + head * sizeof(AccessLog);
  EEPROM.put(addr, logEntry);
  
  head = (head + 1) % MAX_HISTORY;
  EEPROM.write(ADDR_HISTORY_HEAD, head);
}

bool obtenerHistorial(int offset, AccessLog &log) {
  uint8_t head = EEPROM.read(ADDR_HISTORY_HEAD);
  if (head >= MAX_HISTORY) head = 0;
  
  // Newest events first
  int idx = (head - 1 - offset + MAX_HISTORY) % MAX_HISTORY;
  int addr = ADDR_HISTORY_DATA + idx * sizeof(AccessLog);
  EEPROM.get(addr, log);
  
  if (log.year == 0 || log.year == 65535) return false;
  return true;
}

void guardarUsuario(int idx, User &u) {
  int addr = ADDR_USERS + idx * USER_SIZE;
  EEPROM.put(addr, u);
  respaldarEnSD("UPDATE", u);
}

void leerUsuario(int idx, User &u) {
  int addr = ADDR_USERS + idx * USER_SIZE;
  EEPROM.get(addr, u);
}

int contarUsuarios() {
  int count = 0;
  User u;
  for (int i = 0; i < MAX_USERS; i++) {
    leerUsuario(i, u);
    if (u.active == 1) count++;
  }
  return count;
}

int buscarPorId(uint16_t id) {
  User u;
  for (int i = 0; i < MAX_USERS; i++) {
    leerUsuario(i, u);
    if (u.active == 1 && u.id == id) return i;
  }
  return -1;
}

int buscarSlotLibre() {
  User u;
  for (int i = 0; i < MAX_USERS; i++) {
    leerUsuario(i, u);
    if (u.active != 1) return i;
  }
  return -1;
}

uint16_t generarId() {
  uint16_t id;
  int intentosGen = 0;
  do {
    id = random(1000, 10000);
    intentosGen++;
    if (intentosGen > 500) return 0;
  } while (buscarPorId(id) != -1);
  return id;
}

void inicializarEEPROM() {
  uint8_t magic = EEPROM.read(ADDR_MAGIC);
  if (magic != EEPROM_MAGIC) {
    for (int i = 0; i < MAX_USERS; i++) {
      User u;
      memset(&u, 0, sizeof(u));
      int addr = ADDR_USERS + i * USER_SIZE;
      EEPROM.put(addr, u);
    }
    
    // Crear admin por defecto
    User admin;
    memset(&admin, 0, sizeof(admin));
    admin.id = 1000;
    strncpy(admin.name, "Admin", NAME_LEN);
    strncpy(admin.password, "1234", PASS_LEN);
    admin.role = 1;
    admin.loggedIn = 0;
    admin.active = 1;
    guardarUsuario(0, admin);

    // Limpiar historial
    EEPROM.write(ADDR_HISTORY_HEAD, 0);
    for (int i = 0; i < MAX_HISTORY; i++) {
      AccessLog emptyLog;
      memset(&emptyLog, 0, sizeof(emptyLog));
      int addr = ADDR_HISTORY_DATA + i * sizeof(AccessLog);
      EEPROM.put(addr, emptyLog);
    }

    EEPROM.write(ADDR_MAGIC, EEPROM_MAGIC);
    EEPROM.write(ADDR_COUNT, 1);
  }
}

// ==================== FUNCIONES OLED ====================

void oledClear(Terminal* t) {
  t->display->clearDisplay();
  t->display->setTextColor(WHITE);
}

void oledTitulo(Terminal* t, const char* txt) {
  t->display->setTextSize(1);
  t->display->setCursor(0, 0);
  t->display->print(txt);
  t->display->drawLine(0, 10, 127, 10, WHITE);
}

void oledTexto(Terminal* t, const char* txt, int x, int y, int size) {
  t->display->setTextSize(size);
  t->display->setCursor(x, y);
  t->display->print(txt);
}

void oledMostrar(Terminal* t) {
  t->display->display();
}

// ==================== FUNCIONES PANEL LED ====================

void setPanelMsg(const char* msg) {
  strncpy(panelMsg, msg, sizeof(panelMsg) - 1);
  panelMsg[sizeof(panelMsg) - 1] = '\0';
  panelNeedsUpdate = true;
}

void actualizarPanel() {
  if (panelNeedsUpdate) {
    panel.displayClear();
    panel.displayText(panelMsg, PA_CENTER, 30, 0, PA_SCROLL_LEFT, PA_SCROLL_LEFT);
    panelNeedsUpdate = false;
  }
  panel.displayAnimate();
}

// ==================== T9 INPUT ====================

char getT9Char(char key, int pressCount) {
  if (key < '0' || key > '9') return key;
  int idx = key - '0';
  const char* chars = t9map[idx];
  int len = strlen(chars);
  if (len == 0) return key;
  return chars[pressCount % len];
}

void procesarT9(Terminal* t, char key) {
  unsigned long ahora = millis();

  if (key == t->lastT9Key && (ahora - t->lastT9Time) < T9_TIMEOUT) {
    t->t9PressCount++;
    if (t->inputBuffer.length() > 0) {
      t->inputBuffer.remove(t->inputBuffer.length() - 1);
    }
    char c = getT9Char(key, t->t9PressCount);
    t->inputBuffer += c;
  } else {
    t->t9PressCount = 0;
    char c = getT9Char(key, 0);
    t->inputBuffer += c;
  }
  t->lastT9Key = key;
  t->lastT9Time = ahora;
}

void resetT9(Terminal* t) {
  t->lastT9Key = 0;
  t->t9PressCount = 0;
  t->lastT9Time = 0;
}

// ==================== PANTALLAS (Pasando Terminal*) ====================

void pantallaReposo(Terminal* t) {
  oledClear(t);
  if (t->tipo == TERMINAL_ENTRADA) {
    oledTexto(t, "ENTRADA", 20, 10, 2);
  } else {
    oledTexto(t, "SALIDA", 25, 10, 2);
  }
  oledTexto(t, "BLOQUEADO", 10, 35, 2);
  oledMostrar(t);
}

void pantallaLoginId(Terminal* t) {
  oledClear(t);
  oledTitulo(t, t->tipo == TERMINAL_ENTRADA ? "INGRESE ID:" : "SALIDA - ID:");
  oledTexto(t, "ID: ", 0, 18, 1);
  t->display->print(t->inputBuffer);
  oledTexto(t, "# = OK  * = Borrar", 0, 47, 1);
  oledMostrar(t);
}

void pantallaLoginPass(Terminal* t) {
  oledClear(t);
  oledTitulo(t, "CONTRASENA:");
  t->display->setTextSize(1);
  t->display->setCursor(0, 18);
  t->display->print("CLAVE: ");
  
  for (unsigned int i = 0; i < t->inputBuffer.length(); i++) {
    if (i == t->inputBuffer.length() - 1 && (millis() - t->lastT9Time) < T9_TIMEOUT && t->lastT9Key != 0) {
      t->display->print(t->inputBuffer[i]);
    } else {
      t->display->print("*");
    }
  }

  t->display->setCursor(0, 32);
  t->display->print("Intentos: ");
  t->display->print(3 - t->intentos);
  oledTexto(t, "# = OK  * = Borrar", 0, 47, 1);
  if (t->modoAlfanumerico) {
    oledTexto(t, "D=Num", 95, 56, 1);
  } else {
    oledTexto(t, "D=Abc", 95, 56, 1);
  }
  oledMostrar(t);
}

void pantallaMenuAdmin(Terminal* t, const char* adminName) {
  oledClear(t);
  oledTitulo(t, "MENU ADMIN");
  t->display->setTextSize(1);
  t->display->setCursor(0, 14);
  t->display->print("Hola, ");
  t->display->print(adminName);
  oledTexto(t, "A:Add B:Del C:List", 0, 26, 1);
  oledTexto(t, "D:Acceso 1:Historial", 0, 38, 1);
  oledTexto(t, "* : Cerrar sesion", 0, 52, 1);
  oledMostrar(t);
}

void pantallaAddName(Terminal* t) {
  oledClear(t);
  oledTitulo(t, "NUEVO USUARIO");
  oledTexto(t, "Nombre:", 0, 14, 1);
  t->display->setCursor(0, 26);
  t->display->print(t->inputBuffer);
  t->display->print("_");
  oledTexto(t, "#=OK *=Borrar D=Abc", 0, 46, 1);
  oledMostrar(t);
}

void pantallaAddPass(Terminal* t) {
  oledClear(t);
  oledTitulo(t, "NUEVA CONTRASENA");
  oledTexto(t, "Clave:", 0, 14, 1);
  t->display->setCursor(0, 26);
  t->display->print(t->inputBuffer);
  t->display->print("_");
  oledTexto(t, "#=OK *=Borrar D=Abc", 0, 46, 1);
  oledMostrar(t);
}

void pantallaAddRole(Terminal* t) {
  oledClear(t);
  oledTitulo(t, "SELECCIONE ROL");
  oledTexto(t, "A = Admin  B = Emp.", 0, 18, 1);
  oledTexto(t, "* = Cancelar", 0, 48, 1);
  oledMostrar(t);
}

void pantallaDelUser(Terminal* t) {
  oledClear(t);
  oledTitulo(t, "ELIMINAR USUARIO");
  oledTexto(t, "ID a eliminar:", 0, 14, 1);
  t->display->setCursor(0, 28);
  t->display->print(t->inputBuffer);
  oledTexto(t, "# = OK  * = Cancel", 0, 48, 1);
  oledMostrar(t);
}

void pantallaListaUsuarios(Terminal* t) {
  oledClear(t);
  oledTitulo(t, "USUARIOS");
  User u;
  int y = 14;
  int shown = 0;
  int skipped = 0;
  for (int i = 0; i < MAX_USERS && shown < 4; i++) {
    leerUsuario(i, u);
    if (u.active == 1) {
      if (skipped < t->listaOffset) {
        skipped++;
        continue;
      }
      t->display->setTextSize(1);
      t->display->setCursor(0, y);
      if (u.id < 10) t->display->print("000");
      else if (u.id < 100) t->display->print("00");
      else if (u.id < 1000) t->display->print("0");
      t->display->print(u.id);
      t->display->print(" ");
      t->display->print(u.name);
      t->display->print(u.role == 1 ? "[A]" : "[E]");
      t->display->print(u.loggedIn ? "*" : "");
      y += 10;
      shown++;
    }
  }
  oledTexto(t, "A/B=Nav *=Salir", 0, 55, 1);
  oledMostrar(t);
}

void pantallaHistorial(Terminal* t) {
  oledClear(t);
  oledTitulo(t, "HISTORIAL ACCESOS");
  
  int y = 14;
  int shown = 0;
  
  for (int i = 0; i < 4; i++) {
    AccessLog log;
    if (!obtenerHistorial(t->listaOffset + i, log)) {
      if (i == 0 && t->listaOffset == 0) {
        oledTexto(t, "Sin registros", 0, 25, 1);
      }
      break;
    }
    
    t->display->setTextSize(1);
    t->display->setCursor(0, y);
    
    if (log.hour < 10) t->display->print("0");
    t->display->print(log.hour);
    t->display->print(":");
    if (log.minute < 10) t->display->print("0");
    t->display->print(log.minute);
    
    t->display->print(" ");
    t->display->print(log.action == 1 ? "IN " : "OUT");
    
    int userIdx = buscarPorId(log.id);
    if (userIdx != -1) {
      User u;
      leerUsuario(userIdx, u);
      t->display->print(u.name);
    } else {
      t->display->print("ID:");
      t->display->print(log.id);
    }
    
    y += 10;
    shown++;
  }
  
  oledTexto(t, "A/B=Nav *=Salir", 0, 56, 1);
  oledMostrar(t);
}

void pantallaResultado(Terminal* t, const char* linea1, const char* linea2) {
  oledClear(t);
  oledTexto(t, linea1, 5, 15, 2);
  if (strlen(linea2) > 0) {
    oledTexto(t, linea2, 5, 38, 1);
  }
  oledMostrar(t);
}

// ==================== ANIMACIONES PUERTA ====================

void animacionAbrirPuerta() {
  int centro = 7;
  digitalWrite(doorLeds[centro], LOW);
  delay(100);
  actualizarPanel(); // Keep panel alive during delays
  for (int i = 1; i <= 7; i++) {
    digitalWrite(doorLeds[centro - i], LOW);
    digitalWrite(doorLeds[centro + i], LOW);
    delay(100);
    actualizarPanel();
  }
}

void animacionCerrarPuerta() {
  int centro = 7;
  for (int i = 7; i >= 1; i--) {
    digitalWrite(doorLeds[centro - i], HIGH);
    digitalWrite(doorLeds[centro + i], HIGH);
    delay(100);
    actualizarPanel();
  }
  digitalWrite(doorLeds[centro], HIGH);
  delay(100);
  actualizarPanel();
}

// ==================== ACCIONES GENERALES ====================

void accesoDirectoPuerta(Terminal* t) {
  // Solo usar con SuperAdmin, no guarda en BD el ingreso (pero logueamos evento si queremos)
  digitalWrite(LED_GRANTED, HIGH);
  tone(BUZZER_PIN, 2000, 500);
  setPanelMsg(">> SUPER ADMIN >>");
  pantallaResultado(t, "ACCESO", "PERMITIDO");
  
  // Opcional: registrar súper admin usando ID falso
  registrarHistorial(9999, 1); 
  
  animacionAbrirPuerta();
  for(int i=0; i<30; i++) { delay(100); actualizarPanel(); }
  animacionCerrarPuerta();
  
  digitalWrite(LED_GRANTED, LOW);
  setPanelMsg("ENTRADA ->   <- SALIDA");
}

void accesoEntrada(Terminal* t, int idx) {
  User u;
  leerUsuario(idx, u);
  u.loggedIn = 1;
  guardarUsuario(idx, u);
  
  registrarHistorial(u.id, 1); // Log Entrada
  
  digitalWrite(LED_GRANTED, HIGH);
  tone(BUZZER_PIN, 2000, 500);
  
  char msg[60];
  snprintf(msg, sizeof(msg), ">> BIENVENIDO %s >>", u.name);
  setPanelMsg(msg);
  pantallaResultado(t, "BIENVENIDO", u.name);
  
  animacionAbrirPuerta();
  for(int i=0; i<30; i++) { delay(100); actualizarPanel(); }
  animacionCerrarPuerta();
  digitalWrite(LED_GRANTED, LOW);
  setPanelMsg("ENTRADA ->   <- SALIDA");
}

void accesoSalida(Terminal* t, int idx) {
  User u;
  leerUsuario(idx, u);
  u.loggedIn = 0;
  guardarUsuario(idx, u);
  
  registrarHistorial(u.id, 0); // Log Salida
  
  digitalWrite(LED_GRANTED, HIGH);
  tone(BUZZER_PIN, 1500, 300);
  
  char msg[60];
  snprintf(msg, sizeof(msg), "<< HASTA LUEGO %s <<", u.name);
  setPanelMsg(msg);
  
  oledClear(t);
  oledTexto(t, "HASTA", 15, 10, 2);
  oledTexto(t, "LUEGO", 15, 30, 2);
  t->display->setTextSize(1);
  t->display->setCursor(5, 50);
  t->display->print(u.name);
  oledMostrar(t);
  
  animacionAbrirPuerta();
  for(int i=0; i<30; i++) { delay(100); actualizarPanel(); }
  animacionCerrarPuerta();
  digitalWrite(LED_GRANTED, LOW);
  setPanelMsg("ENTRADA ->   <- SALIDA");
}

void alertaSesionActiva(Terminal* t, bool intentandoEntrar) {
  setPanelMsg("!! ALERTA !!");

  for (int i = 0; i < 8; i++) {
    digitalWrite(LED_DENIED, HIGH);
    digitalWrite(LED_ALARM, HIGH);
    tone(BUZZER_PIN, 1500);

    oledClear(t);
    if (intentandoEntrar) {
      oledTexto(t, "YA ESTAS", 5, 10, 2);
      oledTexto(t, "DENTRO!", 15, 35, 2);
    } else {
      oledTexto(t, "YA ESTAS", 5, 10, 2);
      oledTexto(t, "FUERA!", 15, 35, 2);
    }
    oledMostrar(t);

    for(int d=0; d<25; d++) { delay(10); actualizarPanel(); }
    digitalWrite(LED_DENIED, LOW);
    digitalWrite(LED_ALARM, LOW);
    noTone(BUZZER_PIN);
    for(int d=0; d<25; d++) { delay(10); actualizarPanel(); }
  }
}

void alertaAccesoDenegado(Terminal* t, const char* motivo) {
  t->intentos++;
  digitalWrite(LED_DENIED, HIGH);
  tone(BUZZER_PIN, 500, 800);
  setPanelMsg("ACCESO DENEGADO");

  oledClear(t);
  oledTexto(t, motivo, 0, 15, 1);
  if (t->intentos < 3) {
    char buf[25];
    snprintf(buf, sizeof(buf), "Intentos: %d/3", t->intentos);
    oledTexto(t, buf, 0, 35, 1);
  }
  oledMostrar(t);

  for(int d=0; d<150; d++) { delay(10); actualizarPanel(); }
  digitalWrite(LED_DENIED, LOW);

  if (t->intentos >= 3) {
    activarAlarmaBloqueo(t);
  }
}

void activarAlarmaBloqueo(Terminal* t) {
  setPanelMsg("TERMINAL BLOQUEADO");

  for (int i = 0; i < 10; i++) {
    oledClear(t);
    oledTexto(t, "TERMINAL", 5, 10, 2);
    oledTexto(t, "BLOQUEADO", 5, 35, 2);
    oledMostrar(t);

    digitalWrite(LED_DENIED, HIGH);
    digitalWrite(LED_ALARM, HIGH);
    tone(BUZZER_PIN, 1500);
    for(int d=0; d<25; d++) { delay(10); actualizarPanel(); }
    digitalWrite(LED_DENIED, LOW);
    digitalWrite(LED_ALARM, LOW);
    noTone(BUZZER_PIN);
    for(int d=0; d<25; d++) { delay(10); actualizarPanel(); }
  }
  irAReposo(t);
}

// ==================== TRANSICIONES DE ESTADO ====================

void irAReposo(Terminal* t) {
  t->estadoActual = REPOSO;
  t->inputBuffer = "";
  t->intentos = 0;
  t->loginUserId = -1;
  t->adminUserIdx = -1;
  t->modoAlfanumerico = false;
  resetT9(t);
  t->listaOffset = 0;

  pantallaReposo(t);
  
  if (termEntrada.estadoActual == REPOSO && termSalida.estadoActual == REPOSO) {
    digitalWrite(LED_GRANTED, LOW);
    digitalWrite(LED_DENIED, LOW);
    digitalWrite(LED_ALARM, LOW);
    setPanelMsg("ENTRADA ->   <- SALIDA");
  }
}

void irALoginId(Terminal* t) {
  t->estadoActual = LOGIN_ID;
  t->inputBuffer = "";
  t->intentos = 0;
  t->modoAlfanumerico = false;
  resetT9(t);
  t->lastActivityTime = millis();
  
  if (t->tipo == TERMINAL_ENTRADA) setPanelMsg("MODO INGRESO");
  else setPanelMsg("MODO SALIDA");
  
  pantallaLoginId(t);
}

void irALoginPass(Terminal* t) {
  t->estadoActual = LOGIN_PASS;
  t->inputBuffer = "";
  t->modoAlfanumerico = false;
  resetT9(t);
  t->lastActivityTime = millis();
  pantallaLoginPass(t);
}

void irAMenuAdmin(Terminal* t, int idx) {
  t->estadoActual = MENU_ADMIN;
  t->adminUserIdx = idx;
  t->inputBuffer = "";
  resetT9(t);
  t->lastActivityTime = millis();
  
  const char* nm = "Admin";
  if (idx == -1) {
    nm = "SUPER_ADMIN";
  } else {
    User u;
    leerUsuario(idx, u);
    nm = u.name;
  }
  setPanelMsg("MODO ADMIN (ENTRADA)");
  pantallaMenuAdmin(t, nm);
}

// ==================== LOGICA DE LOGIN ====================

void procesarConfirmId(Terminal* t) {
  if (t->inputBuffer.length() == 0) return;

  // VERIFICACION: CODIGO SECRETO "A000" para SUPER ADMIN
  if (t->tipo == TERMINAL_ENTRADA && t->inputBuffer == "A000") {
     irAMenuAdmin(t, -1);
     return;
  }

  uint16_t id = t->inputBuffer.toInt();
  int idx = buscarPorId(id);

  if (idx == -1) {
    alertaAccesoDenegado(t, "ID NO ENCONTRADO");
    if (t->intentos < 3) {
      t->inputBuffer = "";
      resetT9(t);
      pantallaLoginId(t);
    }
    return;
  }

  t->loginUserId = idx;
  irALoginPass(t);
}

void procesarConfirmPass(Terminal* t) {
  if (t->inputBuffer.length() == 0) return;

  User u;
  leerUsuario(t->loginUserId, u);

  if (strcmp(t->inputBuffer.c_str(), u.password) != 0) {
    alertaAccesoDenegado(t, "CLAVE INCORRECTA");
    if (t->intentos < 3) {
      t->inputBuffer = "";
      resetT9(t);
      pantallaLoginPass(t);
    }
    return;
  }

  // Clave correcta
  t->intentos = 0;

  if (t->tipo == TERMINAL_ENTRADA) {
    if (u.role == 1) {
      // Admin ingresa y ve menu
      irAMenuAdmin(t, t->loginUserId);
    } else {
      if (u.loggedIn == 1) {
        alertaSesionActiva(t, true);
        irAReposo(t);
      } else {
        accesoEntrada(t, t->loginUserId);
        irAReposo(t);
      }
    }
  } else {
    // SALIDA
    if (u.loggedIn == 0) {
      alertaSesionActiva(t, false);
      irAReposo(t);
    } else {
      accesoSalida(t, t->loginUserId);
      irAReposo(t);
    }
  }
}

// ==================== LOGICA ADMIN ====================

void adminAgregarUsuario(Terminal* t) {
  t->estadoActual = ADMIN_ADD_NAME;
  t->inputBuffer = "";
  t->modoAlfanumerico = true;
  resetT9(t);
  t->lastActivityTime = millis();
  pantallaAddName(t);
}

void adminConfirmName(Terminal* t) {
  if (t->inputBuffer.length() == 0 || t->inputBuffer.length() > 12) {
    oledClear(t);
    oledTexto(t, "Nombre", 0, 15, 1);
    oledTexto(t, "1-12 chars", 0, 30, 1);
    oledMostrar(t);
    delay(1000);
    t->inputBuffer = "";
    pantallaAddName(t);
    return;
  }
  strncpy(t->tempName, t->inputBuffer.c_str(), NAME_LEN - 1);
  t->tempName[NAME_LEN - 1] = '\0';
  t->estadoActual = ADMIN_ADD_PASS;
  t->inputBuffer = "";
  t->modoAlfanumerico = true;
  resetT9(t);
  pantallaAddPass(t);
}

void adminConfirmPass(Terminal* t) {
  if (t->inputBuffer.length() == 0 || t->inputBuffer.length() > 8) {
    oledClear(t);
    oledTexto(t, "Clave", 0, 15, 1);
    oledTexto(t, "1-8 chars", 0, 30, 1);
    oledMostrar(t);
    delay(1000);
    t->inputBuffer = "";
    pantallaAddPass(t);
    return;
  }
  strncpy(t->tempPass, t->inputBuffer.c_str(), PASS_LEN - 1);
  t->tempPass[PASS_LEN - 1] = '\0';
  t->estadoActual = ADMIN_ADD_ROLE;
  t->inputBuffer = "";
  resetT9(t);
  pantallaAddRole(t);
}

void adminConfirmRole(Terminal* t, uint8_t role) {
  int slot = buscarSlotLibre();
  if (slot == -1) {
    oledClear(t);
    oledTexto(t, "BASE LLENA", 5, 20, 2);
    oledTexto(t, "Max 20", 0, 45, 1);
    oledMostrar(t);
    delay(2000);
    irAMenuAdmin(t, t->adminUserIdx);
    return;
  }

  uint16_t newId = generarId();
  if (newId == 0) {
    oledClear(t);
    oledTexto(t, "ERROR ID", 10, 25, 2);
    oledMostrar(t);
    delay(2000);
    irAMenuAdmin(t, t->adminUserIdx);
    return;
  }

  User newUser;
  memset(&newUser, 0, sizeof(newUser));
  newUser.id = newId;
  strncpy(newUser.name, t->tempName, NAME_LEN);
  strncpy(newUser.password, t->tempPass, PASS_LEN);
  newUser.role = role;
  newUser.loggedIn = 0;
  newUser.active = 1;
  guardarUsuario(slot, newUser);

  oledClear(t);
  oledTitulo(t, "USUARIO CREADO!");
  t->display->setTextSize(1);
  t->display->setCursor(0, 16);
  t->display->print("N: "); t->display->print(newUser.name);
  t->display->setCursor(0, 28);
  t->display->print("ID: ");
  if (newId < 10) t->display->print("000");
  else if (newId < 100) t->display->print("00");
  else if (newId < 1000) t->display->print("0");
  t->display->print(newId);
  t->display->setCursor(0, 40);
  t->display->print("Rol: "); t->display->print(role == 1 ? "ADMIN" : "EMP");
  oledTexto(t, "Presione #", 0, 54, 1);
  oledMostrar(t);

  setPanelMsg("NUEVO USUARIO");
  while (true) {
    char k = t->keypad->getKey();
    if (k == '#' || k == '*') break;
    actualizarPanel();
  }
  irAMenuAdmin(t, t->adminUserIdx);
}

void adminEliminarUsuario(Terminal* t) {
  t->estadoActual = ADMIN_DEL;
  t->inputBuffer = "";
  t->modoAlfanumerico = false;
  resetT9(t);
  pantallaDelUser(t);
}

void adminConfirmDel(Terminal* t) {
  if (t->inputBuffer.length() == 0) return;

  uint16_t id = t->inputBuffer.toInt();
  int idx = buscarPorId(id);

  if (idx == -1) {
    oledClear(t);
    oledTexto(t, "ID NO ENCONTRADO", 0, 35, 1);
    oledMostrar(t);
    delay(1500);
    irAMenuAdmin(t, t->adminUserIdx);
    return;
  }

  if (idx == t->adminUserIdx && t->adminUserIdx != -1) {
    oledClear(t);
    oledTexto(t, "NO PUEDE", 5, 10, 2);
    oledTexto(t, "ELIMINARSE", 0, 35, 1);
    oledMostrar(t);
    delay(1500);
    irAMenuAdmin(t, t->adminUserIdx);
    return;
  }

  User u;
  leerUsuario(idx, u);
  u.active = 0;
  u.loggedIn = 0;
  guardarUsuario(idx, u);

  oledClear(t);
  oledTexto(t, "ELIMINADO", 5, 15, 2);
  t->display->setTextSize(1);
  t->display->setCursor(0, 40);
  t->display->print(u.name);
  oledMostrar(t);
  delay(1500);
  irAMenuAdmin(t, t->adminUserIdx);
}

void adminListarUsuarios(Terminal* t) {
  t->estadoActual = ADMIN_LIST;
  t->listaOffset = 0;
  pantallaListaUsuarios(t);
}

void adminEntrarSalir(Terminal* t) {
    if (t->adminUserIdx == -1) {
        accesoDirectoPuerta(t);
    } else {
        User u;
        leerUsuario(t->adminUserIdx, u);
        if (u.loggedIn == 0) {
            accesoEntrada(t, t->adminUserIdx);
        } else {
            accesoSalida(t, t->adminUserIdx);
        }
    }
    irAReposo(t);
}

void procesarTerminal(Terminal* t) {
  if (t->estadoActual != REPOSO && (millis() - t->lastActivityTime) > INACTIVITY_TIMEOUT) {
    irAReposo(t);
  }

  if (t->modoAlfanumerico) {
    if (t->lastT9Key != 0 && (millis() - t->lastT9Time) >= T9_TIMEOUT) {
      t->lastT9Key = 0;
      t->t9PressCount = 0;
      if (t->estadoActual == LOGIN_PASS) pantallaLoginPass(t);
      if (t->estadoActual == ADMIN_ADD_NAME) pantallaAddName(t);
      if (t->estadoActual == ADMIN_ADD_PASS) pantallaAddPass(t);
    }
  }

  switch (t->estadoActual) {
    case REPOSO: {
      if (digitalRead(t->pirPin) == HIGH) irALoginId(t);
      break;
    }
    case LOGIN_ID: {
      char key = t->keypad->getKey();
      if (key) {
        t->lastActivityTime = millis();
        tone(BUZZER_PIN, 1000, 50);
        if (key == '*') {
          if (t->inputBuffer.length() > 0) t->inputBuffer.remove(t->inputBuffer.length() - 1);
          pantallaLoginId(t);
        } else if (key == '#') {
          procesarConfirmId(t);
        } else if ((key >= '0' && key <= '9') || key == 'A') {
          if (t->inputBuffer.length() < 4) t->inputBuffer += key;
          pantallaLoginId(t);
        }
      }
      break;
    }
    case LOGIN_PASS: {
      char key = t->keypad->getKey();
      if (key) {
        t->lastActivityTime = millis();
        tone(BUZZER_PIN, 1000, 50);
        if (key == '*') {
          if (t->inputBuffer.length() > 0) { t->inputBuffer.remove(t->inputBuffer.length() - 1); resetT9(t); }
          pantallaLoginPass(t);
        } else if (key == '#') {
          procesarConfirmPass(t);
        } else if (key == 'D') {
          t->modoAlfanumerico = !t->modoAlfanumerico;
          resetT9(t);
          pantallaLoginPass(t);
        } else if (key >= '0' && key <= '9') {
          if (t->inputBuffer.length() < 8) {
            if (t->modoAlfanumerico) procesarT9(t, key); else t->inputBuffer += key;
          }
          pantallaLoginPass(t);
        } else if (key >= 'A' && key <= 'C') {
          if (t->inputBuffer.length() < 8) t->inputBuffer += (char)(key + 32);
          pantallaLoginPass(t);
        }
      }
      break;
    }
    case MENU_ADMIN: {
      char key = t->keypad->getKey();
      if (key) {
        t->lastActivityTime = millis();
        tone(BUZZER_PIN, 1000, 50);
        switch (key) {
          case 'A': adminAgregarUsuario(t); break;
          case 'B': adminEliminarUsuario(t); break;
          case 'C': adminListarUsuarios(t); break;
          case 'D': adminEntrarSalir(t); break;
          case '1': t->estadoActual = ADMIN_HISTORY; t->listaOffset = 0; pantallaHistorial(t); break;
          case '*': irAReposo(t); break;
        }
      }
      break;
    }
    case ADMIN_ADD_NAME: {
      char key = t->keypad->getKey();
      if (key) {
        t->lastActivityTime = millis();
        tone(BUZZER_PIN, 1000, 50);
        if (key == '*') {
          if (t->inputBuffer.length() > 0) { t->inputBuffer.remove(t->inputBuffer.length() - 1); resetT9(t); }
          else { irAMenuAdmin(t, t->adminUserIdx); break; }
          pantallaAddName(t);
        } else if (key == '#') {
          adminConfirmName(t);
        } else if (key == 'D') {
          t->modoAlfanumerico = !t->modoAlfanumerico; resetT9(t); pantallaAddName(t);
        } else if (key >= '0' && key <= '9') {
          if (t->inputBuffer.length() < 12) { if (t->modoAlfanumerico) procesarT9(t, key); else t->inputBuffer += key; }
          pantallaAddName(t);
        } else if (key >= 'A' && key <= 'C') {
          if (t->inputBuffer.length() < 12) t->inputBuffer += (char)(key + 32); pantallaAddName(t);
        }
      }
      break;
    }
    case ADMIN_ADD_PASS: {
      char key = t->keypad->getKey();
      if (key) {
        t->lastActivityTime = millis();
        tone(BUZZER_PIN, 1000, 50);
        if (key == '*') {
          if (t->inputBuffer.length() > 0) { t->inputBuffer.remove(t->inputBuffer.length() - 1); resetT9(t); }
          else { t->estadoActual = ADMIN_ADD_NAME; t->inputBuffer = String(t->tempName); pantallaAddName(t); break; }
          pantallaAddPass(t);
        } else if (key == '#') {
          adminConfirmPass(t);
        } else if (key == 'D') {
          t->modoAlfanumerico = !t->modoAlfanumerico; resetT9(t); pantallaAddPass(t);
        } else if (key >= '0' && key <= '9') {
          if (t->inputBuffer.length() < 8) { if (t->modoAlfanumerico) procesarT9(t, key); else t->inputBuffer += key; }
          pantallaAddPass(t);
        } else if (key >= 'A' && key <= 'C') {
          if (t->inputBuffer.length() < 8) t->inputBuffer += (char)(key + 32); pantallaAddPass(t);
        }
      }
      break;
    }
    case ADMIN_ADD_ROLE: {
      char key = t->keypad->getKey();
      if (key) {
         t->lastActivityTime = millis();
         tone(BUZZER_PIN, 1000, 50);
         if (key == 'A') adminConfirmRole(t, 1);       
         else if (key == 'B') adminConfirmRole(t, 0);  
         else if (key == '*') irAMenuAdmin(t, t->adminUserIdx);
      }
      break;
    }
    case ADMIN_DEL: {
      char key = t->keypad->getKey();
      if (key) {
         t->lastActivityTime = millis();
         tone(BUZZER_PIN, 1000, 50);
         if (key == '*') irAMenuAdmin(t, t->adminUserIdx);
         else if (key == '#') adminConfirmDel(t);
         else if (key >= '0' && key <= '9') {
           if (t->inputBuffer.length() < 4) t->inputBuffer += key;
           pantallaDelUser(t);
         }
      }
      break;
    }
    case ADMIN_LIST: {
      char key = t->keypad->getKey();
      if (key) {
         t->lastActivityTime = millis();
         tone(BUZZER_PIN, 1000, 50);
         if (key == '*') irAMenuAdmin(t, t->adminUserIdx);
         else if (key == 'A') { if (t->listaOffset > 0) t->listaOffset--; pantallaListaUsuarios(t); }
         else if (key == 'B') { t->listaOffset++; pantallaListaUsuarios(t); }
      }
      break;
    }
    case ADMIN_HISTORY: {
      char key = t->keypad->getKey();
      if (key) {
         t->lastActivityTime = millis();
         tone(BUZZER_PIN, 1000, 50);
         if (key == '*') irAMenuAdmin(t, t->adminUserIdx);
         else if (key == 'A') { if (t->listaOffset > 0) t->listaOffset--; pantallaHistorial(t); }
         else if (key == 'B') { if (t->listaOffset < MAX_HISTORY - 4) t->listaOffset++; pantallaHistorial(t); }
      }
      break;
    }
  }
}

// ==================== SETUP ====================

void setup() {
  Serial.begin(9600);
  randomSeed(analogRead(A5));

  if (!rtc.begin()) {
    Serial.println("No se encontro RTC DS1307");
    rtcActivo = false;
  } else {
    rtcActivo = true;
    if (!rtc.isrunning()) {
      Serial.println("RTC parado, ajustando..");
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }
  }

  if (SD.begin(SD_CS_PIN)) {
    Serial.println("SD Inicializada.");
    sdActiva = true;
  } else {
    Serial.println("Reintente SD desconectada");
  }

  pinMode(PIR_ENTRADA_PIN, INPUT);
  pinMode(PIR_SALIDA_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_DENIED, OUTPUT);
  pinMode(LED_GRANTED, OUTPUT);
  pinMode(LED_ALARM, OUTPUT);

  for (int i = 0; i < 15; i++) {
    pinMode(doorLeds[i], OUTPUT);
    digitalWrite(doorLeds[i], HIGH);
  }

  delay(100);
  if (!oledEntrada.begin(SSD1306_SWITCHCAPVCC, 0x3D)) {
    Serial.println(F("OLED Entrada fallo"));
    for (;;);
  }
  
  delay(100);
  if (!oledSalida.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("OLED Salida fallo"));
    for (;;);
  }

  panel.begin();
  panel.setIntensity(5);
  panel.displayClear();

  termEntrada.tipo = TERMINAL_ENTRADA;
  termEntrada.display = &oledEntrada;
  termEntrada.keypad = &keypadEntrada;
  termEntrada.pirPin = PIR_ENTRADA_PIN;

  termSalida.tipo = TERMINAL_SALIDA;
  termSalida.display = &oledSalida;
  termSalida.keypad = &keypadSalida;
  termSalida.pirPin = PIR_SALIDA_PIN;

  inicializarEEPROM();

  irAReposo(&termEntrada);
  irAReposo(&termSalida);
  
  setPanelMsg("ENTRADA ->   <- SALIDA");

  Serial.println(F("Sistema listo Dual"));
}

// ==================== LOOP PRINCIPAL ====================

void loop() {
  actualizarPanel();

  if (!sdActiva && (millis() - lastSDFatalTime > SD_RETRY_INTERVAL)) {
     if(SD.begin(SD_CS_PIN)) {
         sdActiva = true;
         Serial.println("SD Reconectada");
     }
     lastSDFatalTime = millis();
  }

  procesarTerminal(&termEntrada);
  procesarTerminal(&termSalida);
}
