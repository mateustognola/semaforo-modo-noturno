// Simulação no PC: finge ser a placa (millis, analogRead, digitalWrite, Serial)
// e inclui o .ino de verdade para testar a lógica.
// Arduino Uno:  g++ -std=c++17 teste/simulacao.cpp -o simulacao && ./simulacao
// ESP32:        g++ -std=c++17 -DARDUINO_ARCH_ESP32 teste/simulacao.cpp -o simulacao && ./simulacao
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cmath>
#include <functional>
#include <map>
#include <string>

#define HIGH 1
#define LOW 0
#define OUTPUT 1
static unsigned long g_now = 0;
static std::function<int(unsigned long)> g_ldr;
static std::map<int,int> g_pins;
static bool g_quiet = true;
unsigned long millis() { return g_now; }
void delay(unsigned long ms) { g_now += ms; }
int analogRead(int) { return g_ldr(g_now); }
void pinMode(int, int) {}
void digitalWrite(int p, int v) { g_pins[p] = v; }
#define A0 14
struct FakeSerial {
  void begin(long) {}
  void print(const char* s) { if (!g_quiet) std::printf("%s", s); }
  void println(const char* s = "") { if (!g_quiet) std::printf("%s\n", s); }
} Serial;

#include "../semaforo_noturno/semaforo_noturno.ino"

// Os sinais foram pensados na escala do ESP32 (0 a 4095, limiares 1200/1600);
// S converte para a escala da placa compilada (no Uno, 0 a 1023, limiares 300/400).
static const double S = LIMIAR_ENTRA_NOITE / 1200.0;
static int noise(int amp) { int a = (int)(amp * S); return (std::rand() % (2 * a + 1)) - a; }
static int sc(double v) { return (int)(v * S); }

static void reset() {
  g_now = 0; g_pins.clear(); modo = NORMAL; fase = FASE_VERMELHO;
  inicioFase = ultimoPisca = ultimaLeituraMs = ultimoLogMs = 0;
  indiceAmostra = 0; somaAmostras = 0; trocasDeModo = 0;
  for (int i = 0; i < N_AMOSTRAS; i++) amostras[i] = 0;
}

struct Result { unsigned trocas; unsigned ingenuo; };

// Roda a simulação por "dur" ms. Conta também quantas trocas um semáforo
// INGÊNUO (um único limiar de 1400, sem média) faria com o mesmo sinal.
static Result run(unsigned long dur) {
  reset(); setup();
  bool ingenuoNoite = false; unsigned ingenuo = 0;
  for (unsigned long t = g_now; t < dur; t++) {
    g_now = t; loop();
    if (t % 50 == 0) {
      bool n = g_ldr(t) < sc(1400);
      if (n != ingenuoNoite) { ingenuo++; ingenuoNoite = n; }
    }
  }
  return {trocasDeModo, ingenuo};
}

int main() {
  std::printf("Placa simulada: %s (limiares %d / %d)\n\n", LIMIAR_ENTRA_NOITE == 1200 ? "ESP32" : "Arduino Uno", LIMIAR_ENTRA_NOITE, LIMIAR_SAI_NOITE);
  std::srand(42);
  int falhas = 0;
  auto check = [&](bool ok, const char* msg) {
    std::printf("[%s] %s\n", ok ? " OK " : "FALHOU", msg); if (!ok) falhas++;
  };

  // 1) Escurece devagar de 2500 até 1050 em 20 s e fica oscilando perto do limiar,
  //    com ruído de ±150 (pior que o real). Deve trocar UMA vez só.
  g_ldr = [](unsigned long t) {
    double base = t < 20000 ? 2500 - (1450.0 * t / 20000) : 1050 + 150 * std::sin(t / 700.0);
    return sc(base) + noise(150);
  };
  Result r = run(60000);
  std::printf("    histerese: %u troca(s) | limiar unico sem media: %u troca(s)\n", r.trocas, r.ingenuo);
  check(r.trocas == 1 && modo == NOTURNO, "Escurecendo devagar com ruido: troca 1 vez e fica no NOTURNO");

  // 2) Luz parada dentro da zona morta: não pode trocar nenhuma vez.
  g_ldr = [](unsigned long) { return sc(1400) + noise(180); };
  r = run(60000);
  std::printf("    histerese: %u troca(s) | limiar unico sem media: %u troca(s)\n", r.trocas, r.ingenuo);
  check(r.trocas == 0, "Luz parada no meio da faixa: nenhuma troca");

  // 3) Ida e volta: escuro de verdade e depois claro de verdade = exatamente 2 trocas.
  g_ldr = [](unsigned long t) { return sc(t < 15000 ? 2500 : t < 30000 ? 600 : 2500) + noise(100); };
  r = run(45000);
  check(r.trocas == 2 && modo == NORMAL, "Escuro e depois claro: 2 trocas, termina no NORMAL");

  // 4) Tempos do plano (RA final 0 -> 5 s / 3 s / 2 s) e ordem das fases.
  g_ldr = [](unsigned long) { return sc(3000); };
  reset(); setup();
  std::string seq; Fase last = fase; unsigned long tFase = 0; bool tempos_ok = true;
  for (unsigned long t = g_now; t < 2 * (TEMPO_VERMELHO_MS + TEMPO_VERDE_MS + TEMPO_AMARELO_MS) + 1000; t++) {
    g_now = t; loop();
    if (fase != last) {
      unsigned long d = t - tFase;
      if (tFase > 0 && d != duracaoDaFase(last)) tempos_ok = false;
      seq += nomeFase(last); seq += ">"; last = fase; tFase = t;
    }
  }
  std::printf("    sequencia: %s...\n", seq.c_str());
  check(seq.rfind("vermelho>verde>amarelo>vermelho>", 0) == 0, "Ordem vermelho -> verde -> amarelo -> vermelho");
  check(tempos_ok, "Cada fase dura exatamente o tempo do plano");

  // 5) Uma luz acesa por vez no NORMAL e amarelo piscando a cada 0,5 s no NOTURNO.
  g_ldr = [](unsigned long) { return sc(500); };
  reset(); setup();
  int inversoes = 0, antes = g_pins[PINO_AMARELO]; bool outras_apagadas = true;
  for (unsigned long t = g_now; t < 10000; t++) {
    g_now = t; loop();
    if (g_pins[PINO_AMARELO] != antes) { inversoes++; antes = g_pins[PINO_AMARELO]; }
    if (g_pins[PINO_VERMELHO] || g_pins[PINO_VERDE]) outras_apagadas = false;
  }
  check(modo == NOTURNO && inversoes >= 19 && inversoes <= 20 && outras_apagadas,
        "Noturno: so o amarelo, invertendo a cada 0,5 s");

  // Mostra como fica o Monitor Serial na transição (cenário 1).
  std::printf("\n--- trecho do Monitor Serial (cenario 1) ---\n");
  std::srand(7);
  g_ldr = [](unsigned long t) {
    double base = t < 20000 ? 2500 - (1450.0 * t / 20000) : 1050 + 150 * std::sin(t / 700.0);
    return sc(base) + noise(150);
  };
  reset(); g_quiet = false; setup(); g_quiet = true;
  for (unsigned long t = g_now; t < 26000; t++) { g_now = t; g_quiet = !(t > 15500 && t < 19500); loop(); }
  g_quiet = true;

  std::printf("\n%s\n", falhas ? "HA FALHAS" : "TODOS OS TESTES PASSARAM");
  return falhas;
}
