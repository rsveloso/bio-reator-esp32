#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <functional>

// --------------------- CONFIG WIFI ------------------------
#define WIFI_SSID_1 "Pollini Agro"
#define WIFI_PASSWORD_1 "Polla1969@"
//
#define WIFI_SSID_2 "Rafa Bia"
#define WIFI_PASSWORD_2 "r4f4b14@"

// --------------------- CONFIG FIREBASE --------------------
#define API_KEY        "AIzaSyBunmzsROUpo8miGHs9lffr70cRwX0cY98"
#define DATABASE_URL   "https://bio-reator-default-rtdb.firebaseio.com"
#define USER_EMAIL     "velosocrew@gmail.com"
#define USER_PASSWORD  "f7bhe301516"

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;
FirebaseData stream; // data object usado para stream

// ------------------- PINOS DOS RELES -------------------------
#define RELE1_PIN 14   // Rele 1 - Bomba1 (temperatura + OD)
#define RELE2_PIN 27   // Rele 2 - Bomba2 (distância)

// ------------------- PINOS conexão Arduino x Esp32 -------------------------
#define RXD2 16
#define TXD2 17

// --------- CONTROLE DA BOMBA 2 / SENSOR DE DISTÂNCIA ---------
bool permitirLeituraDistancia = false;   // Controle independente da bomba 2
float distanciaMin = 30.0;
float distanciaMax = 200.0;
float distanciaRecebida = 0.0;
bool bomba2Ligada = false;

bool bomba2Enchendo = false;            // indica se está no modo enchimento
bool bomba2Controle = false;

// --------- CONTROLE DA BOMBA 1 (Multiplicação) ---------
// -------------------- VARIÁVEIS CONTROLE MULTIPLICAÇÂO ----------------------
bool multiplicacaoAtiva = false;  // padrão
bool multiplicacaoAtivaAnterior = false;
String statusSistema = "desligado";

// novo: sessão de multiplicação (controle de duração em horas)
bool multiplicacaoSessaoAtiva = false;
unsigned long inicioMultiplicacaoMillis = 0;
unsigned long multiplicacaoDuracaoMin = 60; // padrão 1 hora (60 minutos)

// -------------------- VARIÁVEIS PARA VALORES RECEBIDOS ----------------------
float temperaturaRecebida = 0.0;
float odRecebido = 0.0;

// -------------------- VARIÁVEIS PADRÃO (configuráveis via Firebase) --------------------
float temperaturaMax = 35.0;
float temperaturaMin = 28.0;

float ODMax = 30.0;
float ODMin = 10.0;

unsigned long minutosLigado = 10;        // Y minutos (quando for ligar por tempo)
unsigned long minutosSemMudanca = 30;    // X minutos -> "X minutos desligada" (quando passar -> ligar Y)

unsigned long periodoAtivacao = 0; // em minutos (quando multiplicacao inicia)

// -------------------- VARIÁVEIS DE TEMPO / FLAGS --------------------
unsigned long ultimoTempoMudanca = 0;
unsigned long inicioBombaTempo = 0;
unsigned long ultimaLeituraFirebase = 0;

unsigned long inicioPeriodoAtivacao = 0;
bool periodoAtivacaoAtivo = false;

unsigned long tempoDesligadaStart = 0;
bool bombeouPorTempo = false;
bool bomba1Ligada = false;

// -------------------- CONTROLE MANUAL (NOVO) --------------------
bool manualRele1Active = false;
bool manualRele1State = false;

bool manualRele2Active = false;
bool manualRele2State = false;

// -------------------- CONFIGURAÇÃO DE RATE-LIMIT / RECUPERAÇÃO --------------------

const unsigned long FIREBASE_STATUS_INTERVAL_MS = 30000;   // enviar leituras ao Firebase a cada 30s
const unsigned long MIN_FIREBASE_WRITE_INTERVAL_MS = 250; // mínimo entre chamadas seguidas ao RTDB (proteção)
const int MAX_FIREBASE_ERROR_BEFORE_RESET = 5;

unsigned long lastAnyFirebaseWrite = 0;
unsigned long lastStatusSend = 0;
int firebaseErrorCount = 0;

// Conversão
unsigned long toMS(unsigned long min) { return min * 60000UL; }
unsigned long toMShours(unsigned long hours) { return hours * 3600000UL; }

// ----- forward
void safeRestartFirebaseStream();
const char* getTokenStatus(firebase_auth_token_status status);

// ----------------- NOVAS VARIÁVEIS/CONST PARA safeCommand -----------------
unsigned long safe_lastWrite = 0;
const unsigned long SAFE_WRITE_INTERVAL_MS = 250; // mesmo que MIN_FIREBASE_WRITE_INTERVAL_MS
const int SAFE_MAX_RETRIES = 3;

// manter o último status do token atualizado pelo callback
firebase_auth_token_status lastTokenStatus = token_status_uninitialized;
// timestamp da última vez que o token esteve READY
unsigned long lastTokenReadyMillis = 0;
// se o token ficar não-ready por mais que este intervalo, forçar restart (30 minutos)
const unsigned long TOKEN_RECOVER_MS = 30UL * 60UL * 1000UL;
// reinício periódico da placa (30 minutos)
unsigned long lastForcedRestartMillis = 0;
const unsigned long PERIODIC_RESTART_MS = 30UL * 60UL * 1000UL; // 30 minutos

// ----------------------- HELPERS FIREBASE (RE-IMPLEMENTADOS) -----------------------
bool safeCommand(std::function<bool(FirebaseData*)> func, const char* path = "") {
  unsigned long now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠ safeCommand: WiFi desconectado, abortando write.");
    return false;
  }

  if (lastTokenStatus != token_status_ready) {
    Serial.printf("⏳ safeCommand: token não pronto (%s). Aguardando...\n", getTokenStatus(lastTokenStatus));
    unsigned long waitStart = millis();
    bool ready = false;
    while (millis() - waitStart < 2000) { // espera até 2s no máximo por token ficar pronto
      if (lastTokenStatus == token_status_ready) { ready = true; break; }
      delay(100);
    }
    if (!ready) {
      Serial.println("❌ safeCommand: token ainda não pronto após timeout. Abortando write.");
      return false;
    }
  }

  if (now - safe_lastWrite < SAFE_WRITE_INTERVAL_MS) {
    unsigned long toWait = SAFE_WRITE_INTERVAL_MS - (now - safe_lastWrite);
    delay(toWait);
  }
  safe_lastWrite = millis();

  int attempt = 0;
  while (attempt < SAFE_MAX_RETRIES) {
    if (func(&fbdo)) {
      firebaseErrorCount = 0; // sucesso -> reset contador de erros globais
      return true;
    }
    Serial.printf("❌ safeCommand erro (%s) path: %s (HTTP %d) tentativa %d\n",
                  fbdo.errorReason().c_str(), path, fbdo.httpCode(), attempt + 1);
    // Se o erro indica token inválido/expirado, reiniciar a placa imediatamente
    String _err = fbdo.errorReason();
    if (_err.indexOf("token is not ready") != -1 || fbdo.httpCode() == -120) {
      Serial.println("⚠ safeCommand detectou erro de token (revogado/expirado) — reiniciando placa.");
      delay(200);
      ESP.restart();
    }
    firebaseErrorCount++;
    attempt++;

    if (firebaseErrorCount >= MAX_FIREBASE_ERROR_BEFORE_RESET) {
      Serial.println("⚠ Muitos erros Firebase — reiniciando conexão Firebase/Stream...");
      safeRestartFirebaseStream();
      return false;
    }

    delay(200 + attempt * 100);
  }

  return false;
}

bool safeSetString(const char* path, const String &value) {
  return safeCommand([&](FirebaseData* fb) {
    return Firebase.RTDB.setString(fb, path, value.c_str());
  }, path);
}

bool safeSetFloat(const char* path, float value) {
  return safeCommand([&](FirebaseData* fb) {
    return Firebase.RTDB.setFloat(fb, path, value);
  }, path);
}

bool safeSetBool(const char* path, bool value) {
  return safeCommand([&](FirebaseData* fb) {
    return Firebase.RTDB.setBool(fb, path, value);
  }, path);
}

bool safeSetInt(const char* path, int value) {
  return safeCommand([&](FirebaseData* fb) {
    return Firebase.RTDB.setInt(fb, path, value);
  }, path);
}

bool safeSetTimestamp(const char* path) {
  return safeCommand([&](FirebaseData* fb) {
    return Firebase.RTDB.setTimestamp(fb, path);
  }, path);
}

// ----------------------- CONEXÃO WIFI COM FALLBACK -----------------------
bool conectarWiFi() {
  Serial.println("\n📡 Tentando conectar na Rede 1...");
  WiFi.disconnect(true);
  WiFi.begin(WIFI_SSID_1, WIFI_PASSWORD_1);

  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < 10000) {
    delay(300);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ Conectado na Rede 1!");
    return true;
  }

  Serial.println("\n⚠ Falha ao conectar à Rede 1. Tentando Rede 2...");

  WiFi.disconnect(true);
  WiFi.begin(WIFI_SSID_2, WIFI_PASSWORD_2);

  inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < 10000) {
    delay(300);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ Conectado na Rede 2!");
    return true;
  }

  Serial.println("\n❌ Não foi possível conectar a nenhuma rede.");
  return false;
}

// ----------------------- CONTROLE BOMBA 1 (Temp + OD) -----------------------
void ligarBomba1(bool porTempo = false, const char* motivo = "") {
  digitalWrite(RELE1_PIN, LOW); // rele ativo em LOW (conforme seu hardware)
  bomba1Ligada = true;
  bombeouPorTempo = porTempo;
  if (porTempo) inicioBombaTempo = millis();
  tempoDesligadaStart = 0;

  safeSetString("/status/bomba1", "LIGADA");
  if (motivo && strlen(motivo) > 0) {
    safeSetString("/status/bomba1_motivo", String(motivo));
    Serial.printf("➡️ Bomba1 LIGADA (porTempo=%d) motivo: %s\n", porTempo ? 1 : 0, motivo);
  } else {
    safeSetString("/status/bomba1_motivo", "Indefinido");
    Serial.printf("➡️ Bomba1 LIGADA (porTempo=%d) motivo: Indefinido\n", porTempo ? 1 : 0);
  }
}

void desligarBomba1(const char* motivo = "") {
  digitalWrite(RELE1_PIN, HIGH);
  bomba1Ligada = false;
  bombeouPorTempo = false;
  tempoDesligadaStart = millis();

  safeSetString("/status/bomba1", "DESLIGADA");
  if (motivo && strlen(motivo) > 0) {
    safeSetString("/status/bomba1_motivo", String(motivo));
    Serial.printf("⛔ Bomba1 DESLIGADA motivo: %s\n", motivo);
  } else {
    safeSetString("/status/bomba1_motivo", "Indefinido");
    Serial.println("⛔ Bomba1 DESLIGADA motivo: Indefinido");
  }
}

// ----------------------- CONTROLE BOMBA 2 (Distância) -----------------------
void ligarBomba2(const char* motivo = "") {
  digitalWrite(RELE2_PIN, LOW);
  bomba2Ligada = true;
  safeSetString("/status/bomba2", "LIGADA");
  if (motivo && strlen(motivo) > 0) safeSetString("/status/bomba2_motivo", String(motivo));
  Serial.println("➡️ Bomba2 LIGADA (distância)");
}

void desligarBomba2(const char* motivo = "") {
  digitalWrite(RELE2_PIN, HIGH);
  bomba2Ligada = false;
  safeSetString("/status/bomba2", "DESLIGADA");
  if (motivo && strlen(motivo) > 0) safeSetString("/status/bomba2_motivo", String(motivo));
  Serial.println("⛔ Bomba2 DESLIGADA (distância)");
}

// ----------- DESLIGAR COMPLETAMENTE O CONTROLE DA BOMBA2 ----------
void desligarControleBomba2() {
  Serial.println("⛔ Controle da Bomba2 DESATIVADO manualmente.");

  if (!manualRele2Active) {
    desligarBomba2("Controle desativado");
  } else {
    safeSetString("/status/bomba2_controle", "DESATIVADO (AUTO) - manual override presente");
    Serial.println("⚠ Controle automático da bomba2 DESATIVADO, mas override manual está presente (não desligando).");
  }

  bomba2Enchendo = false;
  permitirLeituraDistancia = false;

  safeSetString("/status/bomba2_controle", "DESATIVADO");
}

// ----------- ATIVAR O CONTROLE DA BOMBA2 NOVAMENTE -----------------
void ativarControleBomba2() {
  Serial.println("➡️ Controle da Bomba2 ATIVADO manualmente.");
  if (!manualRele2Active) {
    permitirLeituraDistancia = true;
    bomba2Enchendo = true;
    safeSetString("/status/bomba2_controle", "ATIVADO");
  } else {
    Serial.println("⚠ Controle automático ativado mas override manual presente (não alterando rele2).");
    safeSetString("/status/bomba2_controle", "ATIVADO (AUTO) - manual override presente");
  }
}

void lerPermitDistanciaFirebase() {
  if (Firebase.RTDB.getBool(&fbdo, "/config/permitirLeituraDistancia")) {
      permitirLeituraDistancia = fbdo.boolData();
  }
}

// Centraliza todo o controle automático/manuel da bomba2
void processBomba2Control() {
  // atualizar flag de leitura de distância a partir do Firebase
  lerPermitDistanciaFirebase();

  // DEBUG
  Serial.printf("DBG bomba2Controle=%d permitirLeituraDistancia=%d manualRele2Active=%d bomba2Ligada=%d dist=%.2f min=%.2f max=%.2f\n",
                bomba2Controle, permitirLeituraDistancia, manualRele2Active, bomba2Ligada,
                distanciaRecebida, distanciaMin, distanciaMax);

  // manter compatibilidade com /config/bomba2_controle (apenas registro de status)
  if (permitirLeituraDistancia) ativarControleBomba2(); else desligarControleBomba2();

  // Controle efetivo
  if (manualRele2Active) {
    if (manualRele2State && !bomba2Ligada) {
      ligarBomba2("Override manual ativo: ligar");
    } else if (!manualRele2State && bomba2Ligada) {
      desligarBomba2("Override manual ativo: desligar");
    }
    return;
  }

  // Se o controle por distância está desligado → força desligado
  if (!permitirLeituraDistancia) {
    if (bomba2Ligada) {
      desligarBomba2("controle desativado");
      safeSetString("/status/bomba2_motivo", "Controle desativado");
    }
    return;
  }

  // Segurança: se acima do máximo, desligar
  if (distanciaRecebida > distanciaMax) {
    if (bomba2Ligada) desligarBomba2("acima do máximo (segurança)");
    return;
  }

  // Regras normais: ligar quando > min, desligar quando <= min
  if (distanciaRecebida > distanciaMin && !bomba2Ligada) {
    ligarBomba2("distância acima do mínimo");
  } else if (distanciaRecebida <= distanciaMin && bomba2Ligada) {
    desligarBomba2("distância abaixo/igual ao mínimo");
  }
}

// -------------------- VALIDA SENSORES ------------------------
bool sensoresValidos() {
  // Validar apenas temperatura e OD inicialmente; distância é opcional
  if (temperaturaRecebida <= 0 || odRecebido <= 0) {
    Serial.println("⚠ ALERTA: Leitura inválida detectada (Temp/OD 0 ou negativo).");
    return false;
  }
  if (temperaturaRecebida < -10 || temperaturaRecebida > 100) {
    Serial.println("⚠ Sensor de temperatura fora do intervalo.");
    return false;
  }
  if (odRecebido < 0 || odRecebido > 1000) {
    Serial.println("⚠ Sensor OD fora do intervalo.");
    return false;
  }
  // se periodoAtivacao e multiplicacaoAtiva, ignorar validação de distância
  // (manter esse comportamento: durante o periodo de ativação forçado, sensores não bloqueiam)
  if (periodoAtivacaoAtivo && multiplicacaoAtiva) {
    Serial.println("⚠ RETORNANDO TRUE (periodo de ativacao ativo)");
    return true;
  } else {
    // validar distância somente se o controle por distância estiver permitindo leituras
    if (permitirLeituraDistancia) {
      if (distanciaRecebida <= 0) {
        Serial.println("⚠ ALERTA: Leitura de distância inválida (0 ou negativo)." );
        if (!manualRele2Active) desligarBomba2("Leitura distancia inválida");
        return false;
      }
      if (distanciaRecebida < 0 || distanciaRecebida > distanciaMax) {
        Serial.println("⚠ Sensor de distância fora do intervalo.");
        if (!manualRele2Active) desligarBomba2("⚠ Sensor de distância fora do intervalo.");
        return false;
      }
    }
  }
  return true;
}

// -------------------- BOTÃO DE SEGURANÇA ------------------------
void desligarTodasBombasPorSeguranca() {
  // Serial.println("🛑 MODO DE SEGURANÇA ATIVO: Todas as bombas desligadas.");

  if (!manualRele1Active) {
    desligarBomba1("Segurança: sistema desligado");
  } else {
    Serial.println("⚠ Bomba1 em override manual — não será desligada pela segurança.");
    safeSetString("/status/bomba1_motivo", "Segurança solicitada, mas override manual presente");
  }

  // if (!manualRele2Active) {
  //   desligarBomba2("Segurança: sistema desligado");
  // } else {
  //   Serial.println("⚠ Bomba2 em override manual — não será desligada pela segurança.");
  //   safeSetString("/status/bomba2_motivo", "Segurança solicitada, mas override manual presente");
  // }

  bombeouPorTempo = false;
  periodoAtivacaoAtivo = false;
  multiplicacaoSessaoAtiva = false;
}

// -------------------- LER CONFIG DO FIREBASE ------------------------
void atualizarConfigFirebase() {
  Serial.println("🔄 Atualizando variáveis de configuração do Firebase...");

  // Temperatura
  if (Firebase.RTDB.getFloat(&fbdo, "/config/temperatura_max")) {
    temperaturaMax = fbdo.floatData();
  } else {
    Serial.println("⚠ /config/temperatura_max não encontrado (mantendo padrão).");
  }

  if (Firebase.RTDB.getFloat(&fbdo, "/config/temperatura_min")) {
    temperaturaMin = fbdo.floatData();
  } else {
    Serial.println("⚠ /config/temperatura_min não encontrado (mantendo padrão).");
  }

  // Tempo
  if (Firebase.RTDB.getInt(&fbdo, "/config/intervalo_ligado")) {
    minutosLigado = fbdo.intData();
  } else {
    Serial.println("⚠ /config/intervalo_ligado não encontrado (mantendo padrão).");
  }

  if (Firebase.RTDB.getInt(&fbdo, "/config/intervalo_sem_mudanca")) {
    minutosSemMudanca = fbdo.intData();
  } else {
    Serial.println("⚠ /config/intervalo_sem_mudanca não encontrado (mantendo padrão).");
  }

  // OD
  if (Firebase.RTDB.getFloat(&fbdo, "/config/od_max")) {
    ODMax = fbdo.floatData();
  } else {
    Serial.println("⚠ /config/od_max não encontrado (mantendo padrão).");
  }

  if (Firebase.RTDB.getFloat(&fbdo, "/config/od_min")) {
    ODMin = fbdo.floatData();
  } else {
    Serial.println("⚠ /config/od_min não encontrado (mantendo padrão).");
  }

  // Distância
  if (Firebase.RTDB.getFloat(&fbdo, "/config/distancia_min")) {
      distanciaMin = fbdo.floatData();
  } else {
      safeSetFloat("/config/distancia_min", distanciaMin);
      Serial.println("Criado /config/distancia_min (padrão)");
  }

  if (Firebase.RTDB.getFloat(&fbdo, "/config/distancia_max")) {
      distanciaMax = fbdo.floatData();
  } else {
      safeSetFloat("/config/distancia_max", distanciaMax);
      Serial.println("Criado /config/distancia_max (padrão)");
  }

  // Controle distância (aceita bool ou string: "ATIVADO"/"DESATIVADO"/"true"/"false")
  if (Firebase.RTDB.getBool(&fbdo, "/config/bomba2_controle")) {
      bomba2Controle = fbdo.boolData();
  } else if (Firebase.RTDB.getString(&fbdo, "/config/bomba2_controle")) {
      String s = fbdo.stringData();
      s.toLowerCase();
      if (s == "ativado" || s == "true" || s == "1") {
        bomba2Controle = true;
      } else {
        bomba2Controle = false;
      }
      // normalize value back to boolean in DB
      safeSetBool("/config/bomba2_controle", bomba2Controle);
      Serial.printf("Normalizado /config/bomba2_controle para bool: %d\n", bomba2Controle);
  } else {
      safeSetBool("/config/bomba2_controle", false);
      bomba2Controle = false;
      Serial.println("Criado /config/bomba2_controle = false");
  }

  if (Firebase.RTDB.getBool(&fbdo, "/config/permitirLeituraDistancia")) {
      permitirLeituraDistancia = fbdo.boolData();
  } else {
      Serial.println("⚠ Campo 'permitirLeituraDistancia' não encontrado. Criando como false.");
      safeSetBool("/config/permitirLeituraDistancia", false);
      permitirLeituraDistancia = false;
  }

  // Periodo de ativacao
  if (Firebase.RTDB.getInt(&fbdo, "/config/periodo_ativacao")) {
    periodoAtivacao = fbdo.intData(); // minutos
  } else {
    safeSetInt("/config/periodo_ativacao", (int)periodoAtivacao);
    Serial.println("Criado /config/periodo_ativacao (padrão 0)");
  }

  // Multiplicacao
  if (Firebase.RTDB.getBool(&fbdo, "/config/multiplicacao")) {
      multiplicacaoAtiva = fbdo.boolData();
  } else {
      Serial.println("⚠ Campo 'multiplicacao' não encontrado. Criando como false.");
      safeSetBool("/config/multiplicacao", false);
      multiplicacaoAtiva = false;
  }

  // Nova config: duração da sessão de multiplicação em minutos (substitui horas)
  if (Firebase.RTDB.getInt(&fbdo, "/config/multiplicacao_duracao_minutos")) {
    multiplicacaoDuracaoMin = (unsigned long)fbdo.intData();
  } else {
    safeSetInt("/config/multiplicacao_duracao_minutos", (int)multiplicacaoDuracaoMin);
    Serial.println("Criado /config/multiplicacao_duracao_minutos (padrão 60 min)");
  }

  // Atualiza status geral
  safeSetString("/status/sistema", multiplicacaoAtiva ? "ligado" : "desligado");

  Serial.println("✔ Configurações atualizadas:");
  Serial.printf("TempMax: %.2f\n", temperaturaMax);
  Serial.printf("TempMin: %.2f\n", temperaturaMin);
  Serial.printf("Ligado(Y): %lu min\n", minutosLigado);
  Serial.printf("Desligada(X): %lu min\n", minutosSemMudanca);
  Serial.printf("ODMax: %.2f\n", ODMax);
  Serial.printf("ODMin: %.2f\n", ODMin);
  Serial.printf("distanciaMin: %.2f\n", distanciaMin);
  Serial.printf("distanciaMax: %.2f\n", distanciaMax);
  Serial.printf("bomba2Controle: %d\n", bomba2Controle);
  Serial.printf("permitirLeituraDistancia: %d\n", permitirLeituraDistancia);
  Serial.printf("periodoAtivacao: %lu min\n", periodoAtivacao);
  Serial.printf("multiplicacao Ativa: %d\n", multiplicacaoAtiva);
  Serial.printf("multiplicacao Duracao (horas): %lu\n", multiplicacaoDuracaoMin);
}

// ---------------------- FIREBASE CALLBACK -------------------------------
void tokenStatusCallback(TokenInfo info) {
  lastTokenStatus = info.status;
  Serial.printf("TokenStatus: %s\n", getTokenStatus(info.status));
  // Se o token entrou em erro (revogado/expirado), tentar reiniciar Firebase e Stream
  if (info.status == token_status_error) {
    Serial.println("⚠ Token em estado ERROR — reiniciando Firebase/Stream para reauth...");
    // pequena espera para evitar chamadas repetidas
    delay(200);
    safeRestartFirebaseStream();
  }
  // registrar o timestamp quando o token estiver pronto
  if (info.status == token_status_ready) {
    lastTokenReadyMillis = millis();
  }
}

const char* getTokenStatus(firebase_auth_token_status status) {
  switch (status) {
    case token_status_uninitialized: return "Uninitialized";
    case token_status_on_signing: return "Signing";
    case token_status_on_refresh: return "Refreshing";
    case token_status_ready: return "Ready";
    case token_status_error: return "Error";
    default: return "Unknown";
  }
}

// ---------------------- GESTÃO E RECONEXÃO FIREBASE/STREAM ------------------
void safeRestartFirebaseStream() {
  Serial.println("🔁 Reiniciando Firebase (begin) e Stream...");

  if (stream.httpConnected()) {
    Firebase.RTDB.endStream(&stream);
    delay(200);
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠ WiFi desconectado ao reiniciar Firebase. Tentando reconectar WiFi...");
    WiFi.disconnect();
    conectarWiFi();
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
      delay(200);
    }
  }

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // inicializar timestamp do reinício periódico
  lastForcedRestartMillis = millis();
  delay(200);

  // inicializar timestamp para evitar restart imediato do token
  lastTokenReadyMillis = millis();

  if (!Firebase.RTDB.beginStream(&stream, "/comandos")) {
    Serial.printf("❌ Erro ao iniciar stream: %s\n", stream.errorReason().c_str());
  } else {
    Serial.println("✔ Stream reiniciado em /comandos");
  }

  firebaseErrorCount = 0;
}

void garantirComandosFirebase() {
  Serial.println("🔍 Verificando existência de /comandos/rele1 e /comandos/rele2...");

  if (!Firebase.RTDB.getString(&fbdo, "/comandos/rele1")) {
    Serial.println("⚠ /comandos/rele1 não existe — criando valor padrão AUTO");
    safeSetString("/comandos/rele1", "AUTO");
  }

  if (!Firebase.RTDB.getString(&fbdo, "/comandos/rele2")) {
    Serial.println("⚠ /comandos/rele2 não existe — criando valor padrão AUTO");
    safeSetString("/comandos/rele2", "AUTO");
  }

  Serial.println("✔ Comandos garantidos no Firebase.");
}

// -------------------- SETUP ------------------------
void setup() {
  Serial.begin(115200);
  Serial2.begin(9600, SERIAL_8N1, RXD2, TXD2);

  pinMode(RELE1_PIN, OUTPUT);
  pinMode(RELE2_PIN, OUTPUT);

  // Garantir estado inicial (reles HIGH = desligado)
  digitalWrite(RELE1_PIN, HIGH);
  digitalWrite(RELE2_PIN, HIGH);

  // WiFi
  Serial.print("Conectando ao WiFi...");
  conectarWiFi();
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
    if (millis() - wifiStart > 15000) {
      Serial.println("\n⚠ Timeout conectando WiFi. Tentando de novo...");
      wifiStart = millis();
      WiFi.disconnect();
      conectarWiFi();
    }
  }
  Serial.println("\nWiFi conectado!");

  // Firebase
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  auth.user.email = USER_EMAIL;
  auth.user.password = USER_PASSWORD;
  config.token_status_callback = tokenStatusCallback;

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  Serial.println("🔥 Firebase inicializado!");

  garantirComandosFirebase();

  ultimoTempoMudanca = millis();
  atualizarConfigFirebase(); // carrega valores do Firebase na inicialização

  // INICIAR STREAM PARA OUVIR COMANDOS DO DASHBOARD
  if (!Firebase.RTDB.beginStream(&stream, "/comandos")) {
    Serial.println("❌ Erro ao iniciar stream!");
    Serial.println(stream.errorReason());
  } else {
    Serial.println("✔ Stream iniciado em /comandos");
  }

  // se multiplicacao já estiver ativa no startup, vamos ativar periodo caso exista
  multiplicacaoAtivaAnterior = multiplicacaoAtiva;
  if (multiplicacaoAtiva) {
    // Tentativa de restaurar sessão ativa a partir do timestamp salvo no Firebase
    unsigned long serverStart = 0;
    if (Firebase.RTDB.getInt(&fbdo, "/status/multiplicacao_inicio")) {
      serverStart = (unsigned long)fbdo.intData();
    } else if (Firebase.RTDB.getFloat(&fbdo, "/status/multiplicacao_inicio")) {
      serverStart = (unsigned long)fbdo.floatData();
    }

    if (serverStart > 0) {
      // obter timestamp atual do servidor (escrever e ler um timestamp temporário)
      if (safeSetTimestamp("/status/_server_now")) {
        delay(200);
        unsigned long serverNow = 0;
        if (Firebase.RTDB.getInt(&fbdo, "/status/_server_now")) serverNow = (unsigned long)fbdo.intData();
        else if (Firebase.RTDB.getFloat(&fbdo, "/status/_server_now")) serverNow = (unsigned long)fbdo.floatData();

        if (serverNow > serverStart) {
          unsigned long elapsed = serverNow - serverStart; // ms
          unsigned long durationMs = toMS(multiplicacaoDuracaoMin);
          if (elapsed < durationMs) {
            multiplicacaoSessaoAtiva = true;
            // ajustar inicioMultiplicacaoMillis para continuar contagem local
            inicioMultiplicacaoMillis = millis() - elapsed;
            Serial.printf("🔔 Restaurada sessão de multiplicação: faltam %lu ms\n", (durationMs - elapsed));
          } else {
            // sessão já expirada
            multiplicacaoSessaoAtiva = false;
            multiplicacaoAtiva = false;
            safeSetBool("/config/multiplicacao", false);
            safeSetString("/status/sistema", "desligado");
            safeSetTimestamp("/status/multiplicacao_finalizada");
            Serial.println("⏳ Sessão de multiplicação já expirou antes do boot — finalizando.");
          }
        }
      }
    } else {
      // sem timestamp no servidor -> iniciar sessão localmente
      multiplicacaoSessaoAtiva = true;
      inicioMultiplicacaoMillis = millis();
      // salvar timestamp de início no servidor
      safeSetTimestamp("/status/multiplicacao_inicio");
      safeSetInt("/status/multiplicacao_duracao_minutos", (int)multiplicacaoDuracaoMin);
      Serial.printf("🔔 Multiplicação já ativa na inicialização: sessão iniciada por %lu horas\n", multiplicacaoDuracaoMin);
    }

    if (periodoAtivacao > 0) {
      periodoAtivacaoAtivo = true;
      inicioPeriodoAtivacao = millis();
      Serial.printf("⏱ Periodo de ativacao iniciado (startup): %lu minutos\n", periodoAtivacao);

      if (odRecebido < ODMax && !bomba1Ligada && !manualRele1Active) {
        ligarBomba1(true, "Periodo de ativacao (inicio - startup)");
      } else if (!(odRecebido < ODMax)) {
        Serial.println("⚠ OD já >= ODMax na inicialização: bomba1 permanecerá desligada mesmo no periodo de ativacao.");
      }
    }
  }
}

// -------------------- LOOP ------------------------
void loop() {
  // Reinício periódico configurado: se passou INTERVAL, reinicia a placa
  if (millis() - lastForcedRestartMillis >= PERIODIC_RESTART_MS) {
    Serial.println("⚠ Reinício periódico: 30 minutos atingidos — reiniciando placa (ESP.restart()).");
    delay(200);
    ESP.restart();
  }
  // ============================================================
  // 🔔 OUVIR COMANDOS DO DASHBOARD (STREAM DO FIREBASE)
  // ============================================================
  Firebase.RTDB.readStream(&stream);

  if (stream.streamAvailable()) {
    String path = stream.dataPath();
    String data = stream.stringData();

    Serial.print("📡 Stream recebido: ");
    Serial.print(path);
    Serial.print(" → ");
    Serial.println(data);

    if (path == "/atualizar") {
      Serial.println("🔥 COMANDO RECEBIDO: Atualizar configurações agora!");
      atualizarConfigFirebase();   // atualiza configurações
      safeSetTimestamp("/status/ultima_atualizacao");
      Serial.println("✔ Configurações atualizadas via comando remoto.");
    }

    if (path == "/bomba2_controle" && data == "desligar") {
      desligarControleBomba2();
    }

    if (path == "/bomba2_controle" && data == "ligar") {
      ativarControleBomba2();
    }

    if (path == "/rele1") {
      if (data.equalsIgnoreCase("LIGAR")) {
        manualRele1Active = true;
        manualRele1State = true;
        ligarBomba1(false, "Comando manual (/comandos/rele1 = LIGAR)");
        safeSetString("/status/rele1_manual", "MANUAL_LIGADA");
        Serial.println("✅ Override manual RELE1: LIGAR (manual ativo)");
      } else if (data.equalsIgnoreCase("DESLIGAR")) {
        manualRele1Active = true;
        manualRele1State = false;
        desligarBomba1("Comando manual (/comandos/rele1 = DESLIGAR)");
        safeSetString("/status/rele1_manual", "MANUAL_DESLIGADA");
        Serial.println("✅ Override manual RELE1: DESLIGAR (manual ativo)");
      } else if (data.equalsIgnoreCase("AUTO")) {
        manualRele1Active = false; // libera controle automático imediatamente
        safeSetString("/status/rele1_manual", "AUTO (manual desligado)");
        Serial.println("🔁 RELE1: modo AUTO (override manual removido). Controle automático retorna imediatamente.");
      } else {
        Serial.println("⚠ Comando desconhecido para /rele1. Use LIGAR / DESLIGAR / AUTO.");
      }
    }

    if (path == "/rele2") {
      if (data.equalsIgnoreCase("LIGAR")) {
        manualRele2Active = true;
        manualRele2State = true;
        ligarBomba2("Comando manual (/comandos/rele2 = LIGAR)");
        safeSetString("/status/rele2_manual", "MANUAL_LIGADA");
        Serial.println("✅ Override manual RELE2: LIGAR (manual ativo)");
      } else if (data.equalsIgnoreCase("DESLIGAR")) {
        manualRele2Active = true;
        manualRele2State = false;
        desligarBomba2("Comando manual (/comandos/rele2 = DESLIGAR)");
        safeSetString("/status/rele2_manual", "MANUAL_DESLIGADA");
        Serial.println("✅ Override manual RELE2: DESLIGAR (manual ativo)");
      } else if (data.equalsIgnoreCase("AUTO")) {
        manualRele2Active = false; // libera controle automático imediatamente
        safeSetString("/status/rele2_manual", "AUTO (manual desligado)");
        // limpar motivo anterior e informar retorno a AUTO
        safeSetString("/status/bomba2_motivo", "AUTO (manual desligado)");
        Serial.println("🔁 RELE2: modo AUTO (override manual removido). Controle automático retorna imediatamente.");
      } else {
        Serial.println("⚠ Comando desconhecido para /rele2. Use LIGAR / DESLIGAR / AUTO.");
      }
    }

  } else {
    if (!stream.httpConnected()) {
      static unsigned long lastTryStream = 0;
      if (millis() - lastTryStream > 15000) { // a cada 15s tentar reiniciar
        lastTryStream = millis();
        Serial.println("⚠ Stream não ativo. Tentando reiniciar stream...");
        safeRestartFirebaseStream();
      }
    }
  }

  // ============================================================
    // Token watchdog: se o token ficar não-ready por muito tempo, forçar restart
    if (lastTokenStatus != token_status_ready) {
      if (lastTokenReadyMillis == 0) lastTokenReadyMillis = millis();
      else if (millis() - lastTokenReadyMillis > TOKEN_RECOVER_MS) {
        Serial.println("⚠ Token não pronto por longo período — forçando restart Firebase/Stream...");
        safeRestartFirebaseStream();
        lastTokenReadyMillis = millis();
      }
    } else {
      lastTokenReadyMillis = millis();
    }

    //  RECEBER DADOS DO ARDUINO UNO
  // ============================================================
  if (Serial2.available()) {
    String linha = Serial2.readStringUntil('\n');

    int i1 = linha.indexOf("Temp=");
    int i2 = linha.indexOf(";", i1);
    if (i1 >= 0 && i2 > i1) temperaturaRecebida = linha.substring(i1 + 5, i2).toFloat();

    i1 = linha.indexOf("OD=");
    i2 = linha.indexOf(";", i1);
    if (i1 >= 0 && i2 > i1) odRecebido = linha.substring(i1 + 3, i2).toFloat();

    i1 = linha.indexOf("Dist=");
    i2 = linha.indexOf(";", i1);
    if (i1 >= 0 && i2 > i1) distanciaRecebida = linha.substring(i1 + 5, i2).toFloat();
  }

  // Print para conferência
  Serial.printf("Temp: %.2f | OD: %.2f | Dist: %.2f\n",
                temperaturaRecebida, odRecebido, distanciaRecebida);

  // Envia para Firebase periodicamente (rate-limited)
  unsigned long now = millis();
  if (now - lastStatusSend >= FIREBASE_STATUS_INTERVAL_MS) {
    lastStatusSend = now;

    if (WiFi.status() == WL_CONNECTED) {
      safeSetFloat("/status/temperatura", temperaturaRecebida);
      safeSetFloat("/status/od", odRecebido);
      safeSetFloat("/status/distancia", distanciaRecebida);
      safeSetTimestamp("/status/ultima_atualizacao");
    } else {
      Serial.println("⚠ WiFi não conectado, pulando envio de status.");
      WiFi.disconnect();
      conectarWiFi();
    }
  }

  // Se multiplicação desligada -> segurança e leitura ocasional do config
  if (!multiplicacaoAtiva) {
    Serial.println("⚠ MODO DESLIGADO — Sistema bloqueado.");

    desligarTodasBombasPorSeguranca();

    if (millis() - ultimaLeituraFirebase > 3000) {
        ultimaLeituraFirebase = millis();
        if (WiFi.status() == WL_CONNECTED) {
          atualizarConfigFirebase();
        } else {
          Serial.println("⚠ WiFi desconectado -> tentarei reconectar.");
          conectarWiFi();
        }
    }

    // garantir que a bomba2 seja processada também quando o sistema estiver desligado
    processBomba2Control();

    delay(500);
    return;
  }

  // Se houve mudança no flag multiplicacao (início), tratar periodo de ativacao e sessão
  if (multiplicacaoAtiva && !multiplicacaoAtivaAnterior) {
    Serial.println("🔔 Multiplicação ativada agora!");

    // iniciar sessão de multiplicação (duração em horas)
    multiplicacaoSessaoAtiva = true;
    inicioMultiplicacaoMillis = millis();
    // salvar timestamp de início no servidor para persistência entre reboots
    safeSetTimestamp("/status/multiplicacao_inicio");
    safeSetInt("/status/multiplicacao_duracao_minutos", (int)multiplicacaoDuracaoMin);
    Serial.printf("⏱ Sessão de multiplicação iniciada por %lu horas.\n", multiplicacaoDuracaoMin);

    // iniciar periodo de ativacao se configurado
    if (periodoAtivacao > 0) {
      periodoAtivacaoAtivo = true;
      inicioPeriodoAtivacao = millis();
      Serial.printf("⏱ Periodo de ativacao iniciado: %lu minutos\n", periodoAtivacao);

      // somente ligar por periodo se não houver override manual em bomba1 E OD permitir
      if (odRecebido < ODMax) {
        if (!bomba1Ligada && !manualRele1Active) ligarBomba1(true, "Periodo de ativacao (inicio)");
      } else {
        Serial.println("⚠ OD >= ODMax no inicio do periodo: bomba1 ficará OFF (prioridade OD).");
        if (!manualRele1Active) desligarBomba1("OD >= ODMax (periodo inicio)");
      }
    }
  }
  multiplicacaoAtivaAnterior = multiplicacaoAtiva;

  // Se sessão de multiplicação ativa -> checar expiração da sessão
  if (multiplicacaoSessaoAtiva) {
    unsigned long tempoSessao = millis() - inicioMultiplicacaoMillis;
    if (tempoSessao >= toMS(multiplicacaoDuracaoMin)) {
      // fim da sessão: set multiplicacao false no Firebase e limpar flags
      multiplicacaoSessaoAtiva = false;
      periodoAtivacaoAtivo = false;
      multiplicacaoAtiva = false;
      safeSetBool("/config/multiplicacao", false);
      safeSetString("/status/sistema", "desligado");
      safeSetTimestamp("/status/multiplicacao_finalizada");
      Serial.println("⏳ Sessão de multiplicação finalizada automaticamente (duracao atingida). multiplicacao setada para FALSE no Firebase.");
      // respeitar override manual: se manualRele1Active true, não alteramos rele físico aqui
      if (!manualRele1Active) {
        desligarBomba1("Sessão de multiplicação finalizada");
      }
      // Após encerrar a sessão, continuar o loop (irá cair no bloco que trata multiplicacaoAtiva == false no próximo ciclo)
      delay(200);
      return;
    }
  }

  // ==========================================
  // 2) Verificar se sensores estão válidos
  // ==========================================
  if (!sensoresValidos()) {
    delay(1000);
    return;
  }

  // Processar controle da bomba2 (funciona também quando multiplicacao inativa)
  processBomba2Control();

  // ======================================================================
  // LÓGICA BOMBA 1 (TEMPERATURA + OD + PERIODOS) - com suporte a periodoAtivacao já existente
  // ======================================================================
  if (manualRele1Active) {
    if (manualRele1State && !bomba1Ligada) {
      ligarBomba1(false, "Override manual ativo: ligar");
    } else if (!manualRele1State && bomba1Ligada) {
      desligarBomba1("Override manual ativo: desligar");
    }
  } else {
      // Se estamos em periodo de ativacao, priorizar esse fluxo e IGNORAR checagens de OD/Temp
      if (periodoAtivacaoAtivo) {
        unsigned long tempoPeriodo = millis() - inicioPeriodoAtivacao;
        if (tempoPeriodo >= toMS(periodoAtivacao)) {
          periodoAtivacaoAtivo = false;
          Serial.println("⏳ Periodo de ativacao finalizado. Voltando às regras normais.");
          // quando periodoAtivacao termina, desligar bomba se foi ligada por periodo e não houver outra regra mantendo ligada
          if (bombeouPorTempo && bomba1Ligada) {
            // desligar somente se não for override manual
            if (!manualRele1Active) {
              desligarBomba1("Periodo de ativacao finalizado");
            }
          }
        } else {
          if (!bomba1Ligada) {
            Serial.println("🔔 Periodo de ativacao ativo -> ligando bomba1 (periodo). (ignora OD/Temp)");
            ligarBomba1(true, "Periodo de ativacao (ativo)");
          }
        }
      } else {
        // Fora do periodo de ativacao -> comportamento normal, mas se estivermos dentro de uma sessao de multiplicacao
        // permitimos que a regra de ociosidade (minutosSemMudanca -> minutosLigado) ignore OD/Temp.

        if (odRecebido >= ODMax) {
          if (!multiplicacaoSessaoAtiva) {
            if (bomba1Ligada) {
              Serial.println("⚠ OD >= ODMax -> Forçando DESLIGAR bomba1 (prioridade OD).");
              desligarBomba1("OD >= ODMax (prioridade)");
            }
            // quando não estamos em sessao de multiplicacao, obedecemos OD normalmente
            // caso estejamos em sessao, pulamos essa restrição
          }
        } else {
          if (temperaturaRecebida < temperaturaMin) {
            if (!multiplicacaoSessaoAtiva) {
              if (bomba1Ligada) {
                Serial.println("Temp abaixo do mínimo -> desligando bomba1.");
                desligarBomba1("Temperatura abaixo do mínimo");
              }
            }
          } else if (temperaturaRecebida > temperaturaMax) {
            if (!multiplicacaoSessaoAtiva) {
              if (!bomba1Ligada) {
                Serial.println("Temp acima do max -> tentando LIGAR bomba1 (OD permite).\");
                ligarBomba1(false, "Temperatura acima do máximo");
              }
            }
          }

          // Agora tratamos a regra de periodo/ociosidade quando não estamos no periodo ativo
          if (!periodoAtivacaoAtivo) {
            if (!bomba1Ligada) {
              if (tempoDesligadaStart == 0) {
                tempoDesligadaStart = millis();
                Serial.println("▶ Contador de tempo desligada da bomba1 iniciado.");
              } else {
                unsigned long tempoDesligadaAtual = millis() - tempoDesligadaStart;
                if (!bomba1Ligada && !bombeouPorTempo && tempoDesligadaAtual >= toMS(minutosSemMudanca)) {
                  // permitir ligar por ociosidade mesmo que OD/Temp estejam fora, se estivermos em sessao de multiplicacao
                  if (multiplicacaoSessaoAtiva || odRecebido < ODMax) {
                    Serial.println("⏱ Bomba1 ficou X minutos desligada -> ligando por Y minutos (regra tempo desligada).");
                    ligarBomba1(true, "Regra: X minutos desligada -> ligar Y minutos");
                  } else {
                    Serial.println("⚠ Tentativa de ligar por tempo bloqueada: OD >= ODMax.");
                  }
                }
              }
            } else {
              if (bombeouPorTempo) {
                unsigned long tempoLigadaAtual = millis() - inicioBombaTempo;
                if (tempoLigadaAtual >= toMS(minutosLigado)) {
                  Serial.println("⏲ Tempo ligado por regra expirou -> desligando bomba1.");
                  desligarBomba1("Tempo ligado por regra expirou");
                }
              } else {
                tempoDesligadaStart = 0;
              }
            }
          }
        }
      }
  }

  // ============================================================
  // ATUALIZA FIREBASE CONFIG UPDATE a cada 10s
  // ============================================================
  if (millis() - ultimaLeituraFirebase > 10000) {
    ultimaLeituraFirebase = millis();
    if (WiFi.status() == WL_CONNECTED) {
      atualizarConfigFirebase();
    } else {
      Serial.println("⚠ WiFi desconectado -> tentando reconectar.");
      conectarWiFi();
    }
  }

  delay(200); // pausa curta para aliviar CPU
}
