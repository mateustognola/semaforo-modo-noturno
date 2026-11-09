/*
 * ============================================================================
 *  Semáforo com dois modos de operação (normal e noturno) — Arduino Uno / ESP32
 *  Atividade Ponderada de Computação 2 — Inteli, Módulo 4, Semana 4
 *  Autor: Mateus Tognola
 * ============================================================================
 *
 *  O semáforo funciona em dois modos:
 *    - NORMAL : ciclo vermelho -> verde -> amarelo, com tempos dados pelo RA.
 *    - NOTURNO: só o amarelo pisca (intermitente), avisando "passe com cuidado".
 *
 *  Quem decide o modo é o LDR. Para o semáforo NÃO ficar trocando de modo sem
 *  parar quando a luz está perto do limite, usamos HISTERESE: existem DOIS
 *  limiares em vez de um.
 *
 *      leitura do LDR (Arduino Uno: 0 a 1023)
 *      1023 ┤  mais luz
 *           │
 *       330 ┤─────────── só VOLTA para o modo NORMAL se passar daqui para cima
 *           │  zona morta: aqui dentro o semáforo MANTÉM o modo em que já está
 *       200 ┤─────────── só ENTRA no modo NOTURNO se cair daqui para baixo
 *           │
 *         0 ┤  menos luz
 *
 *  Como o ruído do sensor é bem menor que a distância entre os dois limiares,
 *  ele nunca consegue empurrar a leitura de um limiar até o outro, então o
 *  modo troca uma vez só e fica estável.
 *
 *  O mesmo código compila no Arduino Uno (placa usada) e no ESP32: os pinos e
 *  os limiares mudam sozinhos conforme a placa escolhida na IDE.
 *
 *  Circuito no Arduino Uno:
 *    5V -> LDR -> A0 -> resistor 10 kΩ -> GND        (mais luz = leitura maior)
 *    pino 8  -> LED vermelho -> 220 Ω -> GND
 *    pino 9  -> LED amarelo  -> 220 Ω -> GND
 *    pino 10 -> LED verde    -> 220 Ω -> GND
 *    (o resistor pode ficar antes ou depois do LED: em série, o efeito é o mesmo)
 *
 *  Circuito no ESP32 (alternativa): 3V3 -> LDR -> GPIO 34 -> 10 kΩ -> GND,
 *  LEDs nos GPIO 16, 17 e 18, limiares 1200 e 1600 (ADC de 0 a 4095).
 * ============================================================================
 */

#include <stdarg.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// 1) PLANO SEMAFÓRICO — troque SÓ o último dígito do seu RA
// ---------------------------------------------------------------------------
const int ULTIMO_DIGITO_RA = 7;  // <<<<<< COLOQUE AQUI O ÚLTIMO DÍGITO DO SEU RA

// Vermelho = 5 + dígito | Verde = 3 + metade do dígito (arredondada para baixo)
// A divisão de inteiros em C++ já descarta a parte decimal (ex.: 7 / 2 = 3).
const unsigned long TEMPO_VERMELHO_MS = (5UL + ULTIMO_DIGITO_RA) * 1000UL;
const unsigned long TEMPO_VERDE_MS    = (3UL + ULTIMO_DIGITO_RA / 2) * 1000UL;
const unsigned long TEMPO_AMARELO_MS  = 2000UL;             // igual para todos
const unsigned long MEIO_PERIODO_PISCA_MS = 500UL;          // 0,5 s aceso / 0,5 s apagado

// ---------------------------------------------------------------------------
// 2) PINOS e 3) HISTERESE — calibre os limiares com o Monitor Serial (ver README)
// ---------------------------------------------------------------------------
#if defined(ARDUINO_ARCH_ESP32)
// ESP32: ADC de 12 bits (0 a 4095)
const int PINO_LDR      = 34;     // ADC1, só entrada
const int PINO_VERMELHO = 16;     // RX2
const int PINO_AMARELO  = 17;     // TX2
const int PINO_VERDE    = 18;
const int LIMIAR_ENTRA_NOITE = 1200;  // média ABAIXO disso -> entra no NOTURNO
const int LIMIAR_SAI_NOITE   = 1600;  // média ACIMA disso  -> volta ao NORMAL
#else
// Arduino Uno: ADC de 10 bits (0 a 1023)
const int PINO_LDR      = A0;
const int PINO_VERMELHO = 8;
const int PINO_AMARELO  = 9;
const int PINO_VERDE    = 10;
const int LIMIAR_ENTRA_NOITE = 200;   // média ABAIXO disso -> entra no NOTURNO (calibrado)
const int LIMIAR_SAI_NOITE   = 330;   // média ACIMA disso  -> volta ao NORMAL (calibrado)
#endif
static_assert(LIMIAR_ENTRA_NOITE < LIMIAR_SAI_NOITE,
              "O limiar de entrada no modo noturno precisa ser MENOR que o de saida");

const long VELOCIDADE_SERIAL = 9600;  // escolha 9600 no Monitor Serial

// Média móvel: suaviza o ruído antes de comparar com os limiares.
const int N_AMOSTRAS = 10;                      // 10 leituras...
const unsigned long INTERVALO_LEITURA_MS = 50;  // ...a cada 50 ms = janela de 0,5 s
const unsigned long INTERVALO_LOG_MS     = 300; // de quanto em quanto tempo imprime

// ---------------------------------------------------------------------------
// Classe Led (mesma ideia da Aula 3): o loop diz O QUE fazer, o objeto sabe COMO
// ---------------------------------------------------------------------------
class Led {
  int pino;
 public:
  Led(int p) : pino(p) {}
  void iniciar()          { pinMode(pino, OUTPUT); desligar(); }
  void ligar()            { digitalWrite(pino, HIGH); }
  void desligar()         { digitalWrite(pino, LOW); }
  void definir(bool aceso){ digitalWrite(pino, aceso ? HIGH : LOW); }
};

Led vermelho(PINO_VERMELHO);
Led amarelo(PINO_AMARELO);
Led verde(PINO_VERDE);

// ---------------------------------------------------------------------------
// Estado do programa
// ---------------------------------------------------------------------------
enum Modo { NORMAL, NOTURNO };
enum Fase { FASE_VERMELHO, FASE_VERDE, FASE_AMARELO };

Modo modo = NORMAL;
Fase fase = FASE_VERMELHO;

unsigned long inicioFase   = 0;  // quando a fase atual do modo normal começou
unsigned long ultimoPisca  = 0;  // última vez que o amarelo inverteu no noturno
bool amareloAceso          = false;

int amostras[N_AMOSTRAS];        // buffer circular da média móvel
int indiceAmostra = 0;
long somaAmostras = 0;
int ultimaLeitura = 0;
int media = 0;

unsigned long ultimaLeituraMs = 0;
unsigned long ultimoLogMs     = 0;
unsigned int trocasDeModo     = 0;  // quantas vezes o modo mudou (prova da estabilidade)

// ---------------------------------------------------------------------------
// Funções auxiliares
// ---------------------------------------------------------------------------
// O Arduino Uno não tem Serial.printf; esta função faz o mesmo papel nas duas placas.
void imprimir(const char* formato, ...) {
  char texto[128];
  va_list args;
  va_start(args, formato);
  vsnprintf(texto, sizeof(texto), formato, args);
  va_end(args);
  Serial.print(texto);
}

const char* nomeModo(Modo m) { return m == NORMAL ? "NORMAL" : "NOTURNO"; }

const char* nomeFase(Fase f) {
  switch (f) {
    case FASE_VERMELHO: return "vermelho";
    case FASE_VERDE:    return "verde";
    default:            return "amarelo";
  }
}

unsigned long duracaoDaFase(Fase f) {
  switch (f) {
    case FASE_VERMELHO: return TEMPO_VERMELHO_MS;
    case FASE_VERDE:    return TEMPO_VERDE_MS;
    default:            return TEMPO_AMARELO_MS;
  }
}

Fase proximaFase(Fase f) {
  switch (f) {
    case FASE_VERMELHO: return FASE_VERDE;
    case FASE_VERDE:    return FASE_AMARELO;
    default:            return FASE_VERMELHO;
  }
}

// Acende só a luz da fase atual.
void mostrarFase() {
  vermelho.definir(fase == FASE_VERMELHO);
  amarelo.definir(fase == FASE_AMARELO);
  verde.definir(fase == FASE_VERDE);
}

// Lê o LDR e atualiza a média móvel (buffer circular: tira a mais antiga, põe a nova).
void lerSensor() {
  ultimaLeitura = analogRead(PINO_LDR);
  somaAmostras -= amostras[indiceAmostra];
  amostras[indiceAmostra] = ultimaLeitura;
  somaAmostras += ultimaLeitura;
  indiceAmostra = (indiceAmostra + 1) % N_AMOSTRAS;
  media = somaAmostras / N_AMOSTRAS;
}

void entrarModoNormal() {
  modo = NORMAL;
  fase = FASE_VERMELHO;      // ao sair do noturno começa pelo vermelho (mais seguro)
  inicioFase = millis();
  mostrarFase();
}

void entrarModoNoturno() {
  modo = NOTURNO;
  vermelho.desligar();
  verde.desligar();
  amareloAceso = true;
  amarelo.ligar();
  ultimoPisca = millis();
}

// ---------------------------------------------------------------------------
// A DECISÃO AVALIADA: histerese com dois limiares
// ---------------------------------------------------------------------------
void atualizarModo() {
  if (modo == NORMAL && media < LIMIAR_ENTRA_NOITE) {
    trocasDeModo++;
    imprimir("\n>>> ESCURECEU: media %d < %d -> MODO NOTURNO (troca %u)\n\n",
                  media, LIMIAR_ENTRA_NOITE, trocasDeModo);
    entrarModoNoturno();
  } else if (modo == NOTURNO && media > LIMIAR_SAI_NOITE) {
    trocasDeModo++;
    imprimir("\n>>> CLAREOU: media %d > %d -> MODO NORMAL (troca %u)\n\n",
                  media, LIMIAR_SAI_NOITE, trocasDeModo);
    entrarModoNormal();
  }
  // Se a média estiver entre os dois limiares, nada acontece: o modo atual é mantido.
}

// Modo normal: troca de fase quando o tempo da fase acaba (millis, sem delay).
void atualizarSemaforo(unsigned long agora) {
  if (agora - inicioFase >= duracaoDaFase(fase)) {
    fase = proximaFase(fase);
    inicioFase = agora;
    mostrarFase();
  }
}

// Modo noturno: amarelo intermitente.
void atualizarPisca(unsigned long agora) {
  if (agora - ultimoPisca >= MEIO_PERIODO_PISCA_MS) {
    ultimoPisca = agora;
    amareloAceso = !amareloAceso;
    amarelo.definir(amareloAceso);
  }
}

void imprimirStatus() {
  const char* zona = media < LIMIAR_ENTRA_NOITE ? "abaixo da faixa"
                   : media > LIMIAR_SAI_NOITE   ? "acima da faixa "
                                                : "DENTRO da faixa";
  if (modo == NORMAL) {
    imprimir("LDR: %4d | media: %4d | %s | NORMAL (%s) | trocas: %u\n",
                  ultimaLeitura, media, zona, nomeFase(fase), trocasDeModo);
  } else {
    imprimir("LDR: %4d | media: %4d | %s | NOTURNO (piscando) | trocas: %u\n",
                  ultimaLeitura, media, zona, trocasDeModo);
  }
}

// ---------------------------------------------------------------------------
// setup e loop
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(VELOCIDADE_SERIAL);
  vermelho.iniciar();
  amarelo.iniciar();
  verde.iniciar();

  // Enche a média móvel antes de decidir o modo inicial.
  for (int i = 0; i < N_AMOSTRAS; i++) {
    amostras[i] = analogRead(PINO_LDR);
    somaAmostras += amostras[i];
    delay(10);  // só na inicialização
  }
  ultimaLeitura = amostras[N_AMOSTRAS - 1];
  media = somaAmostras / N_AMOSTRAS;

  Serial.println();
  Serial.println("=== Semaforo com modo noturno (histerese) ===");
  imprimir("Plano do RA final %d: vermelho %lus | verde %lus | amarelo %lus\n",
                ULTIMO_DIGITO_RA, TEMPO_VERMELHO_MS / 1000, TEMPO_VERDE_MS / 1000,
                TEMPO_AMARELO_MS / 1000);
  imprimir("Entra no noturno se media < %d | volta ao normal se media > %d\n\n",
                LIMIAR_ENTRA_NOITE, LIMIAR_SAI_NOITE);

  // Modo inicial: só começa no noturno se já estiver claramente escuro.
  if (media < LIMIAR_ENTRA_NOITE) entrarModoNoturno();
  else                            entrarModoNormal();
}

void loop() {
  unsigned long agora = millis();

  // 1) Sensor: lê e decide o modo (sempre, mesmo no meio de um vermelho longo)
  if (agora - ultimaLeituraMs >= INTERVALO_LEITURA_MS) {
    ultimaLeituraMs = agora;
    lerSensor();
    atualizarModo();
  }

  // 2) Luzes: executa o modo atual
  if (modo == NORMAL) atualizarSemaforo(millis());
  else                atualizarPisca(millis());

  // 3) Log no Monitor Serial
  if (agora - ultimoLogMs >= INTERVALO_LOG_MS) {
    ultimoLogMs = agora;
    imprimirStatus();
  }
}
