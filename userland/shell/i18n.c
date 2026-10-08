#include "i18n.h"

static i18n_lang g_lang = LANG_EN;

static const char *EN[S_COUNT] = {
    "EchOS  -  locked",
    "User",
    "Password",
    "Enter password to unlock",
    "Wrong password — try again",
    "Welcome",
    "Install EchOS",
    "Language",
    "Locale",
    "Time zone",
    "Date and time",
    "Set the clock to the time on your phone. It is written to the CMOS RTC.",
    "Your account",
    "Username",
    "Confirm password",
    "Install disk",
    "Erase disk and install",
    "Installing…",
    "Installation complete",
    "Remove the USB stick and reboot. EchOS will start from this disk.",
    "Continue on this LIVE session",
    "Back",
    "Next",
    "Skip (use LIVE only)",
    "Keep current time",
    "Password cannot be empty",
    "Passwords do not match",
    "No disk was found (AHCI / virtio / IDE).",
    "Disk is too small (need 16 MiB).",
    "LIVE image has no kernel payload — disk data will persist, USB still needed to boot.",
};

static const char *ES[S_COUNT] = {
    "EchOS  -  bloqueado",
    "Usuario",
    "Contraseña",
    "Introduce la contraseña para desbloquear",
    "Contraseña incorrecta — inténtalo de nuevo",
    "Bienvenido",
    "Instalar EchOS",
    "Idioma",
    "Configuración regional",
    "Zona horaria",
    "Fecha y hora",
    "Pon la hora que ves en el móvil. Se escribe en el reloj CMOS.",
    "Tu cuenta",
    "Nombre de usuario",
    "Confirmar contraseña",
    "Disco de instalación",
    "Borrar el disco e instalar",
    "Instalando…",
    "Instalación terminada",
    "Saca el USB y reinicia. EchOS arrancará desde este disco.",
    "Seguir en esta sesión LIVE",
    "Atrás",
    "Siguiente",
    "Omitir (solo LIVE)",
    "Conservar la hora actual",
    "La contraseña no puede estar vacía",
    "Las contraseñas no coinciden",
    "No hay disco (AHCI / virtio / IDE).",
    "El disco es demasiado pequeño (hacen falta 16 MiB).",
    "Esta LIVE no trae el núcleo — los datos se guardan, pero hará falta el USB para arrancar.",
};

void i18n_set(i18n_lang lang) {
    g_lang = (lang == LANG_ES) ? LANG_ES : LANG_EN;
}

i18n_lang i18n_get(void) { return g_lang; }

const char *i18n_code(void) { return g_lang == LANG_ES ? "es" : "en"; }

bool i18n_set_code(const char *code) {
    if (!code) return false;
    if ((code[0] == 'e' || code[0] == 'E') && (code[1] == 's' || code[1] == 'S')) {
        i18n_set(LANG_ES);
        return true;
    }
    if ((code[0] == 'e' || code[0] == 'E') && (code[1] == 'n' || code[1] == 'N')) {
        i18n_set(LANG_EN);
        return true;
    }
    return false;
}

const char *T(int id) {
    if (id < 0 || id >= S_COUNT) return "";
    return (g_lang == LANG_ES ? ES : EN)[id];
}
