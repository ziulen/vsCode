#include <Arduino.h>
#include <Preferences.h>
Preferences preferencias;
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
// LABORATORIO DE CUADRATURA
// =====================================================

const int MAX_ESTADOS_Q = 10;

int tiempoCuadratura = 10;
int ciclosCuadratura = 1;

uint8_t secuenciaCuadratura[MAX_ESTADOS_Q][2];

int cantidadEstadosQ = 5;
// =====================================================
// PROTOTIPOS
// =====================================================

void ejecutarCuadratura(bool invertir);
void estadoLaboratorio(bool a, bool b);
bool cargarSecuenciaCuadratura(String texto);
void guardarConfiguracionCuadratura();
bool cargarConfiguracionGuardada();
// =====================================================
// ENCODER
// =====================================================

int estadoAnterior = 0;
int acumulador = 0;
int contadorUpWPF = 0;
int contadorDownWPF = 0;
// =====================================================
// PASO LÓGICO WPF
// =====================================================

const int PASO_MINIMO = 0;
const int PASO_MAXIMO = 20;

int pasoActual = 0;
// =====================================================
// TEMPORIZADOR PARA VOLTAJE/RPM
// =====================================================

unsigned long ultimoEnvioVoltaje = 0;

const unsigned long INTERVALO_VOLTAJE = 500;
void guardarConfiguracionCuadratura()
{
    preferencias.begin("cuadratura", false);

    preferencias.putInt(
        "tiempo",
        tiempoCuadratura);

    preferencias.putInt(
        "ciclos",
        ciclosCuadratura);

    preferencias.putInt(
        "cantidad",
        cantidadEstadosQ);

    preferencias.putBytes(
        "secuencia",
        secuenciaCuadratura,
        sizeof(secuenciaCuadratura));

    preferencias.end();

    Serial.println("QSAVE_OK");
}bool cargarConfiguracionGuardada()
{
    preferencias.begin("cuadratura", true);

    bool existe =
        preferencias.isKey("tiempo") &&
        preferencias.isKey("ciclos") &&
        preferencias.isKey("cantidad") &&
        preferencias.isKey("secuencia");

    if (!existe)
    {
        preferencias.end();
        return false;
    }

    int tiempo =
        preferencias.getInt("tiempo", 10);

    int ciclos =
        preferencias.getInt("ciclos", 1);

    int cantidad =
        preferencias.getInt("cantidad", 5);


    // Validación básica
    if (tiempo < 1 ||
        tiempo > 1000 ||
        ciclos < 1 ||
        ciclos > 20 ||
        cantidad < 2 ||
        cantidad > MAX_ESTADOS_Q)
    {
        preferencias.end();
        return false;
    }


    uint8_t temporal[MAX_ESTADOS_Q][2] = {};

    size_t bytes =
        preferencias.getBytes(
            "secuencia",
            temporal,
            sizeof(temporal));

    preferencias.end();


    if (bytes != sizeof(temporal))
    {
        return false;
    }


    tiempoCuadratura = tiempo;
    ciclosCuadratura = ciclos;
    cantidadEstadosQ = cantidad;

    memcpy(
        secuenciaCuadratura,
        temporal,
        sizeof(secuenciaCuadratura));


    Serial.println("QLOAD_OK");

    return true;
}
// =====================================================
// GENERAR ESTADO HACIA LA PLACA
// =====================================================

void estado(bool a, bool b)
{
    digitalWrite(SALIDA_A, a ? HIGH : LOW);
    digitalWrite(SALIDA_B, b ? HIGH : LOW);

    delay(10);
}
void estadoLaboratorio(bool a, bool b)
{
    digitalWrite(SALIDA_A, a ? HIGH : LOW);
    digitalWrite(SALIDA_B, b ? HIGH : LOW);

    delay(tiempoCuadratura);
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
void mostrarPasoFan()
{
    uint32_t milivoltios =
        leerMilivoltiosFan();

    float voltajeGPIO =
        milivoltios / 1000.0f;

    // Divisor 20K / 10K
    float voltajeFan =
        voltajeGPIO * 3.0f;

    Serial.print("PASO:");
    Serial.print(pasoActual);

    Serial.print(" | GPIO34:");
    Serial.print(voltajeGPIO, 3);

    Serial.print(" V | FAN:");
    Serial.print(voltajeFan, 3);

    Serial.println(" V");
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
bool cargarSecuenciaCuadratura(String texto)
{
    texto.trim();

    int cantidad = 0;
    int inicio = 0;

    while (inicio < texto.length())
    {
        int coma =
            texto.indexOf(',', inicio);

        String estado;

        if (coma == -1)
        {
            estado =
                texto.substring(inicio);
        }
        else
        {
            estado =
                texto.substring(
                    inicio,
                    coma);
        }

        estado.trim();


        if (estado.length() != 2)
        {
            return false;
        }


        char a = estado.charAt(0);
        char b = estado.charAt(1);


        if ((a != '0' && a != '1') ||
            (b != '0' && b != '1'))
        {
            return false;
        }


        if (cantidad >= MAX_ESTADOS_Q)
        {
            return false;
        }


        secuenciaCuadratura[cantidad][0] =
            a - '0';

        secuenciaCuadratura[cantidad][1] =
            b - '0';


        cantidad++;


        if (coma == -1)
        {
            break;
        }


        inicio = coma + 1;
    }


    if (cantidad < 2)
    {
        return false;
    }


    cantidadEstadosQ = cantidad;

    return true;
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

    // ==========================================
    // USAR CONFIGURACIÓN DEL LAB
    // ==========================================

    // false = ejecutar secuencia tal como
    // fue configurada desde WPF
    ejecutarCuadratura(false);

    pasoActual++;

    // Esperar estabilización
    delay(500);

    Serial.print("OK UP #");
    Serial.println(contadorUpWPF);

    mostrarPasoFan();
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

    // ==========================================
    // USAR CONFIGURACIÓN DEL LAB
    // ==========================================

    // true = ejecutar la misma secuencia
    // configurada desde WPF, pero al revés
    ejecutarCuadratura(true);

    if (pasoActual > 0)
    {
        pasoActual--;
    }

    // Esperar estabilización
    delay(500);

    Serial.print("OK DOWN #");
    Serial.println(contadorDownWPF);

    mostrarPasoFan();
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

    mostrarPasoFan();
}

else if (comando.startsWith("QTIME:"))
{
    int valor =
        comando.substring(6).toInt();

    if (valor >= 1 &&
        valor <= 1000)
    {
        tiempoCuadratura = valor;

        Serial.print("QTIME_OK:");
        Serial.println(tiempoCuadratura);
    }
    else
    {
        Serial.println("QTIME_ERROR");
    }
}
else if (comando.startsWith("CYCLES:"))
{
    int valor =
        comando.substring(7).toInt();

    if (valor >= 1 &&
        valor <= 20)
    {
        ciclosCuadratura = valor;

        Serial.print("CYCLES_OK:");
        Serial.println(ciclosCuadratura);
    }
    else
    {
        Serial.println("CYCLES_ERROR");
    }
}
else if (comando.startsWith("QSEQ:"))
{
    String texto =
        comando.substring(5);


    if (cargarSecuenciaCuadratura(texto))
    {
        Serial.print("QSEQ_OK:");

        for (int i = 0;
             i < cantidadEstadosQ;
             i++)
        {
            Serial.print(
                secuenciaCuadratura[i][0]);

            Serial.print(
                secuenciaCuadratura[i][1]);

            if (i <
                cantidadEstadosQ - 1)
            {
                Serial.print(",");
            }
        }

        Serial.println();
    }
    else
    {
        Serial.println("QSEQ_ERROR");
    }
}
else if (comando == "QSAVE")
{
    guardarConfiguracionCuadratura();
}

else if (comando == "TESTUP")
{
    ejecutarCuadratura(false);

    delay(500);

    Serial.println("TESTUP_OK");

    mostrarPasoFan();
}
else if (comando == "TESTDOWN")
{
    ejecutarCuadratura(true);

    delay(500);

    Serial.println("TESTDOWN_OK");

    mostrarPasoFan();
}
    // =====================================================
    // COMANDO DESCONOCIDO
    // =====================================================

    else
    {
        Serial.println("ERROR");
    }
}

void ejecutarCuadratura(bool invertir)
{
    for (int ciclo = 0;
         ciclo < ciclosCuadratura;
         ciclo++)
    {
        if (!invertir)
        {
            for (int i = 0;
                 i < cantidadEstadosQ;
                 i++)
            {
                estadoLaboratorio(
                    secuenciaCuadratura[i][0],
                    secuenciaCuadratura[i][1]);
            }
        }
        else
        {
            for (int i = cantidadEstadosQ - 1;
                 i >= 0;
                 i--)
            {
                estadoLaboratorio(
                    secuenciaCuadratura[i][0],
                    secuenciaCuadratura[i][1]);
            }
        }
    }
}
// =====================================================
// SETUP
// =====================================================

void setup()
{
    Serial.begin(115200);


    // =======================================  ==========
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
    // =====================================================
// =====================================================
// CONFIGURACIÓN DE CUADRATURA POR DEFECTO
// =====================================================

tiempoCuadratura = 10;
ciclosCuadratura = 1;
cantidadEstadosQ = 5;

secuenciaCuadratura[0][0] = 0;
secuenciaCuadratura[0][1] = 0;

secuenciaCuadratura[1][0] = 0;
secuenciaCuadratura[1][1] = 1;

secuenciaCuadratura[2][0] = 1;
secuenciaCuadratura[2][1] = 1;

secuenciaCuadratura[3][0] = 1;
secuenciaCuadratura[3][1] = 0;

secuenciaCuadratura[4][0] = 0;
secuenciaCuadratura[4][1] = 0;


// =====================================================
// INTENTAR CARGAR CONFIGURACIÓN GUARDADA
// =====================================================

if (cargarConfiguracionGuardada())
{
    Serial.println("CONFIG CUADRATURA: GUARDADA");
}
else
{
    Serial.println("CONFIG CUADRATURA: DEFAULT");
}
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