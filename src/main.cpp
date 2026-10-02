#include <Arduino.h>
// =====================================================
// CONTROL COMPLETO DEL COOLER - ESP32
//
// Encoder físico:
//   A   -> GPIO32
//   B   -> GPIO33
//   COM -> GND
//
// Salidas hacia placa:
//   GPIO25 -> 4K7 -> Q1 -> PAD A
//   GPIO27 -> 4K7 -> Q2 -> PAD B
//
// Medición FAN:
//   FAN+ -> 20K -> GPIO34 -> 10K -> GND
//
// Comunicación:
//   USB / Serial -> WPF
//   115200 baud
// =====================================================


// =====================================================
// PINES
// =====================================================

// Salidas hacia los transistores
const int SALIDA_A = 25;
const int SALIDA_B = 27;

// Encoder físico
const int ENCODER_A = 32;
const int ENCODER_B = 33;

// ADC voltaje FAN
const int PIN_VOLTAJE = 34;

// Relé botón ON/OFF
const int RELE_POWER = 26;
// =====================================================
// DIVISOR DE VOLTAJE
// =====================================================

const float R1 = 20000.0;   // FAN+ -> GPIO34
const float R2 = 10000.0;   // GPIO34 -> GND


// =====================================================
// ENCODER
// =====================================================

int estadoAnterior = 0;
int acumulador = 0;
int contadorUpWPF = 0;
int contadorDownWPF = 0;

// =====================================================
// TEMPORIZADOR PARA VOLTAJE/RPM
// =====================================================

unsigned long ultimoEnvioVoltaje = 0;

const unsigned long INTERVALO_VOLTAJE = 500;


// =====================================================
// GENERAR ESTADO HACIA LA PLACA
// =====================================================

void estado(bool a, bool b)
{
    // =====================================================
    // LOS TRANSISTORES 2N2222 INVIERTEN LA SEÑAL
    //
    // Queremos PAD = 1
    //      ↓
    // transistor OFF
    //      ↓
    // GPIO = LOW
    //
    // Queremos PAD = 0
    //      ↓
    // transistor ON
    //      ↓
    // GPIO = HIGH
    // =====================================================

    digitalWrite(SALIDA_A, a ? HIGH : LOW);
    digitalWrite(SALIDA_B, b ? HIGH : LOW);

    delay(10);
}

// =====================================================
// SUBIR
// =====================================================

void subir()
{
    estado(LOW,  LOW);   // PAD 11
    estado(HIGH, LOW);   // PAD 01
    estado(HIGH, HIGH);  // PAD 00
    estado(LOW,  HIGH);  // PAD 10
    estado(LOW,  LOW);   // PAD 11
}


// =====================================================
// BAJAR
// =====================================================

void bajar()
{
    estado(LOW,  LOW);   // PAD 11
    estado(LOW,  HIGH);  // PAD 10
    estado(HIGH, HIGH);  // PAD 00
    estado(HIGH, LOW);   // PAD 01
    estado(LOW,  LOW);   // PAD 11
}

// =====================================================
// ENVIAR ESTADO ENCODER
// =====================================================

void enviarEstadoEncoder()
{
    int A = digitalRead(ENCODER_A);
    int B = digitalRead(ENCODER_B);

    Serial.print("ENC:");
    Serial.print(A);
    Serial.print(",");
    Serial.println(B);
}


// =====================================================
// LEER ENCODER
// =====================================================

void leerEncoder()
{
    int A = digitalRead(ENCODER_A);
    int B = digitalRead(ENCODER_B);

    int estadoActual = (A << 1) | B;

    // No hubo cambio
    if (estadoActual == estadoAnterior)
        return;

    // Tabla de cuadratura
    static const int8_t tabla[16] =
    {
         0, -1,  1,  0,
         1,  0,  0, -1,
        -1,  0,  0,  1,
         0,  1, -1,  0
    };

    int indice =
        (estadoAnterior << 2) | estadoActual;

    int movimiento =
        tabla[indice];

    estadoAnterior = estadoActual;

    // Transición inválida
    if (movimiento == 0)
        return;

    // Acumular cuadratura
    acumulador += movimiento;

    // ==========================================
    // CICLO COMPLETO SENTIDO POSITIVO
    //
    // 11 -> 01 -> 00 -> 10 -> 11
    // ==========================================

    if (acumulador >= 4)
    {
        acumulador = 0;

        Serial.println("ENCODER:UP");

        // Reproducir exactamente el ciclo
        // hacia la placa
        subir();

        return;
    }

    // ==========================================
    // CICLO COMPLETO SENTIDO NEGATIVO
    //
    // 11 -> 10 -> 00 -> 01 -> 11
    // ==========================================

    if (acumulador <= -4)
    {
        acumulador = 0;

        Serial.println("ENCODER:DOWN");

        // Reproducir exactamente el ciclo
        // contrario hacia la placa
        bajar();

        return;
    }

    // Protección por ruido extraño
    if (acumulador > 8 || acumulador < -8)
    {
        acumulador = 0;
    }
}
// =====================================================
// LEER ADC RAW GPIO34
// =====================================================
uint32_t leerMilivoltiosFan()
{
    const int MUESTRAS = 64;
    uint32_t suma = 0;

    for (int i = 0; i < MUESTRAS; i++)
    {
        suma += analogReadMilliVolts(PIN_VOLTAJE);

        // Pequeña separación entre muestras
        delayMicroseconds(500);
    }

    return suma / MUESTRAS;
}

// =====================================================
// ENVIAR ADC RAW
// =====================================================
void enviarDatosFan()
{
    uint32_t milivoltios = leerMilivoltiosFan();

    Serial.print("MV:");
    Serial.println(milivoltios);
}

// =====================================================
// BOTÓN POWER - RELÉ
// =====================================================

void pulsarPower()
{
    Serial.println("POWER:ON");

    // Relé activo en LOW
    digitalWrite(RELE_POWER, LOW);

    // Mantener pulsación durante 3 segundos
    delay(3000);

    // Soltar
    digitalWrite(RELE_POWER, HIGH);

    Serial.println("POWER:OFF");
}


// =====================================================
// PROCESAR COMANDO WPF
// =====================================================

void procesarSerial()
{
    if (Serial.available() <= 0)
        return;

    String comando = Serial.readStringUntil('\n');
    comando.trim();


    // =====================================================
    // SUBIR RPM
    // WPF: botón +
    //
    // En tu placa la secuencia bajar() es la que SUBE RPM.
    //
    // 20 clics WPF:
    // 20 ciclos normales
    // + 4 ciclos extra (clics 5,10,15,20)
    // = 24 ciclos eléctricos
    // =====================================================

    if (comando == "UP")
    {
        contadorUpWPF++;
        contadorDownWPF = 0;

        // Un paso normal
        bajar();

        // Paso adicional cada 5 clics
        if (contadorUpWPF % 5 == 0)
        {
            delay(20);

            bajar();

            Serial.println("EXTRA UP");
        }

        Serial.print("OK UP #");
        Serial.println(contadorUpWPF);
    }


    // =====================================================
    // BAJAR RPM
    // WPF: botón -
    //
    // En tu placa la secuencia subir() es la que BAJA RPM.
    //
    // 20 clics WPF:
    // 20 ciclos normales
    // + 4 ciclos extra
    // = 24 ciclos eléctricos
    // =====================================================

    else if (comando == "DOWN")
    {
        contadorDownWPF++;
        contadorUpWPF = 0;

        // Un paso normal
        subir();

        // Paso adicional cada 5 clics
        if (contadorDownWPF % 5 == 0)
        {
            delay(20);

            subir();

            Serial.println("EXTRA DOWN");
        }

        Serial.print("OK DOWN #");
        Serial.println(contadorDownWPF);
    }


    // =====================================================
    // POWER
    // =====================================================

    else if (comando == "POWER")
    {
        pulsarPower();

        Serial.println("OK POWER");
    }


    // =====================================================
    // STATUS
    // =====================================================

    else if (comando == "STATUS")
    {
        enviarEstadoEncoder();
    }


    // =====================================================
    // COMANDO DESCONOCIDO
    // =====================================================

    else
    {
        Serial.println("ERROR");
    }
}


// =====================================================
// SETUP
// =====================================================

void setup()
{
    Serial.begin(115200);


    // =================================================
    // SALIDAS TRANSISTORES
    // =================================================

    pinMode(SALIDA_A, OUTPUT);
    pinMode(SALIDA_B, OUTPUT);


    // Transistores apagados inicialmente

    digitalWrite(SALIDA_A, LOW);
    digitalWrite(SALIDA_B, LOW);

    // =================================================
// RELÉ POWER
// =================================================

pinMode(RELE_POWER, OUTPUT);

// HIGH = relé apagado
digitalWrite(RELE_POWER, HIGH);
    // =================================================
    // ENCODER
    // =================================================

    pinMode(ENCODER_A, INPUT_PULLUP);
    pinMode(ENCODER_B, INPUT_PULLUP);


    // =================================================
    // ADC
    // =================================================

    pinMode(PIN_VOLTAJE, INPUT);

    analogReadResolution(12);


    delay(500);


    // =================================================
    // ESTADO INICIAL ENCODER
    // =================================================

    int A = digitalRead(ENCODER_A);
    int B = digitalRead(ENCODER_B);

    estadoAnterior =
        (A << 1) | B;


    // =================================================
    // LISTO
    // =================================================

    Serial.println("ESP32 LISTO");

    enviarEstadoEncoder();

    enviarDatosFan();
}


// =====================================================
// LOOP
// =====================================================

void loop()
{
    // =================================================
    // 1. ENCODER FÍSICO
    // =================================================

    leerEncoder();


    // =================================================
    // 2. WPF
    // =================================================

    procesarSerial();


    // =================================================
    // 3. VOLTAJE / RPM
    //
    // Enviar cada 500 ms
    // =================================================

    unsigned long ahora = millis();


    if (ahora - ultimoEnvioVoltaje >= INTERVALO_VOLTAJE)
    {
       ultimoEnvioVoltaje = ahora;

        enviarDatosFan();
    }
}