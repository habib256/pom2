// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core_options.hpp"

#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace pom2::libretro {
namespace {

struct Choice {
    const char* value;
    const char* en;
    const char* fr;
};

struct Category {
    const char* key;
    const char* en;
    const char* fr;
    const char* infoEn;
    const char* infoFr;
};

struct Option {
    const char* key;
    const char* category;
    const char* en;
    const char* fr;
    const char* infoEn;
    const char* infoFr;
    std::vector<Choice> choices;   // the first one is the default
};

const Category kCategories[] = {
    {"system", "System", "Système",
     "Machine, processor and video standard. Changes apply when the content is reloaded.",
     "Machine, processeur et standard vidéo. Les changements s'appliquent au rechargement du contenu."},
    {"video", "Video", "Vidéo",
     "Monitor and RGB card.", "Moniteur et carte RVB."},
    {"audio", "Audio", "Audio",
     "Speaker and Mockingboard.", "Haut-parleur et Mockingboard."},
    {"input", "Input", "Entrées",
     "Joystick, paddles, keyboard and mouse.", "Joystick, paddles, clavier et souris."},
    {"media", "Disks", "Disques",
     "Disk drives, write protection and saving.", "Lecteurs, protection en écriture et sauvegarde."},
};

std::vector<Choice> percent(int from, int to, int step, int first)
{
    static std::deque<std::string> storage;   // never moves its strings: stable c_str()
    std::vector<Choice> out;
    const auto add = [&](int value) {
        storage.push_back(std::to_string(value) + "%");
        out.push_back({storage.back().c_str(), nullptr, nullptr});
    };
    add(first);
    for (int value = from; value <= to; value += step)
        if (value != first) add(value);
    return out;
}

const std::vector<Option>& options()
{
    static const std::vector<Option> table = {
        // ---- System ----------------------------------------------------
        {"pom2_machine", "system", "Machine (restart)", "Machine (redémarrage)",
         "Automatic picks the machine a WOZ disk names, then the first one whose firmware is in "
         "system/pom2: //e Enhanced, //e, ][+, //c.",
         "Automatique choisit la machine qu'indique un disque WOZ, puis la première dont le firmware "
         "est dans system/pom2 : //e Enhanced, //e, ][+, //c.",
         {{"auto", "Automatic", "Automatique"},
          {"iie-enhanced", "Apple //e Enhanced (65C02)", "Apple //e Enhanced (65C02)"},
          {"iie", "Apple //e (6502)", "Apple //e (6502)"},
          {"iiplus", "Apple ][+", "Apple ][+"},
          {"ii", "Apple ][ (1977)", "Apple ][ (1977)"},
          {"iic", "Apple //c", "Apple //c"},
          {"iic-plus", "Apple //c Plus", "Apple //c Plus"}}},
        {"pom2_video_standard", "system", "Video standard (restart)", "Standard vidéo (redémarrage)",
         "NTSC is 60 Hz at 1.0227 MHz. PAL is the European 50 Hz machine at 1.0156 MHz, needed by "
         "French Touch demos. Automatic selects PAL for DIX.",
         "NTSC : 60 Hz à 1,0227 MHz. PAL : la machine européenne à 50 Hz et 1,0156 MHz, nécessaire "
         "aux démos French Touch. Automatique choisit PAL pour DIX.",
         {{"auto", "Automatic", "Automatique"}, {"ntsc", "NTSC (60 Hz)", "NTSC (60 Hz)"},
          {"pal", "PAL (50 Hz)", "PAL (50 Hz)"}}},
        {"pom2_cpu", "system", "Processor (restart)", "Processeur (redémarrage)",
         "Replace the machine's 6502 on a ][, ][+ or //e. The //c family keeps its soldered 65C02.",
         "Remplace le 6502 d'un ][, ][+ ou //e. Les //c gardent leur 65C02 soudé.",
         {{"auto", "Machine's own", "Celui de la machine"}, {"nmos", "NMOS 6502", "NMOS 6502"},
          {"cmos", "65C02", "65C02"}}},
        {"pom2_accelerator", "system", "Accelerator (restart)", "Accélérateur (redémarrage)",
         "A faster 6502 in the socket. Automatic gives the //c Plus its built-in 4 MHz. The Zip Chip runs "
         "at 4 MHz, the FASTChip up to 16.6 MHz; programs can slow them, and Esc held at reset starts a "
         "//c Plus at 1 MHz.",
         "Un 6502 plus rapide dans le support. Automatique donne au //c Plus ses 4 MHz intégrés. La Zip "
         "Chip tourne à 4 MHz, la FASTChip jusqu'à 16,6 MHz ; les programmes peuvent les ralentir, et Échap "
         "maintenue au reset démarre un //c Plus à 1 MHz.",
         {{"auto", "Automatic", "Automatique"}, {"none", "None (1 MHz)", "Aucun (1 MHz)"},
          {"zip", "Zip Chip (4 MHz)", "Zip Chip (4 MHz)"}, {"fastchip", "FASTChip (16.6 MHz)", "FASTChip (16,6 MHz)"}}},
        {"pom2_ramworks", "system", "Auxiliary memory (restart)", "Mémoire auxiliaire (redémarrage)",
         "A RamWorks III in the //e's auxiliary slot: AppleWorks, ProDOS RAM disks and some demos use the "
         "extra banks. Save states grow with it.",
         "Une RamWorks III dans le slot auxiliaire du //e : AppleWorks, les RAM disks ProDOS et certaines "
         "démos utilisent les banques en plus. Les sauvegardes d'état grossissent avec elle.",
         {{"64k", "64 KiB (standard)", "64 Kio (standard)"}, {"256k", "RamWorks 256 KiB", "RamWorks 256 Kio"},
          {"512k", "RamWorks 512 KiB", "RamWorks 512 Kio"}, {"1m", "RamWorks 1 MiB", "RamWorks 1 Mio"},
          {"3m", "RamWorks 3 MiB", "RamWorks 3 Mio"}, {"8m", "RamWorks 8 MiB", "RamWorks 8 Mio"}}},
        {"pom2_clock", "system", "No-Slot Clock (restart)", "Horloge No-Slot Clock (redémarrage)",
         "A DS1216E clock under the ROM gives ProDOS and many programs the date. Emulated time starts at "
         "the host's date when the content loads and follows the machine, so save states and netplay "
         "replay the same date.",
         "Une horloge DS1216E sous la ROM donne la date à ProDOS et à de nombreux programmes. Le temps "
         "émulé part de la date de l'hôte au chargement et suit la machine : sauvegardes d'état et netplay "
         "retrouvent la même date.",
         {{"emulated", "Emulated time", "Temps émulé"}, {"host", "Host clock", "Horloge de l'hôte"},
          {"disabled", "Absent", "Absente"}}},
        {"pom2_dix_preset", "system", "DIX preset (restart)", "Préréglage DIX (redémarrage)",
         "French Touch's DIX anthology: a PAL //e Enhanced with a Mockingboard in slot 2 and a Le Chat "
         "Mauve RGB card. Automatic recognizes the published DIX images.",
         "L'anthologie DIX de French Touch : un //e Enhanced PAL avec Mockingboard en slot 2 et carte "
         "RVB Le Chat Mauve. Automatique reconnaît les images DIX publiées.",
         {{"auto", "Automatic", "Automatique"}, {"enabled", "Always", "Toujours"},
          {"disabled", "Never", "Jamais"}}},

        // ---- Video -----------------------------------------------------
        {"pom2_display", "video", "Display", "Affichage",
         "Composite modes reproduce the colours of an NTSC monitor. OpenEmulator and AppleWin decode "
         "the composite signal. Automatic uses the RGB card's monitor when one is present.",
         "Les modes composite reproduisent les couleurs d'un moniteur NTSC. OpenEmulator et AppleWin "
         "décodent le signal composite. Automatique utilise le moniteur de la carte RVB si elle est présente.",
         {{"auto", "Automatic", "Automatique"},
          {"composite", "Composite colour", "Couleur composite"},
          {"composite-medium", "Composite, medium colours", "Composite, couleurs moyennes"},
          {"composite-sharp", "Composite, sharp", "Composite, net"},
          {"composite-oe", "Composite signal (OpenEmulator)", "Signal composite (OpenEmulator)"},
          {"applewin-monitor", "NTSC monitor (AppleWin)", "Moniteur NTSC (AppleWin)"},
          {"applewin-tv", "NTSC television (AppleWin)", "Téléviseur NTSC (AppleWin)"},
          {"applewin-idealized", "NTSC idealized colours", "NTSC, couleurs idéalisées"},
          {"mono-white", "Monochrome white", "Monochrome blanc"},
          {"mono-green", "Monochrome green", "Monochrome vert"},
          {"mono-amber", "Monochrome amber", "Monochrome ambre"},
          {"rgb", "RGB (Le Chat Mauve card)", "RVB (carte Le Chat Mauve)"}}},
        {"pom2_rgb_card", "video", "Le Chat Mauve RGB card (restart)", "Carte RVB Le Chat Mauve (redémarrage)",
         "The Féline in slot 7 of a ][+ or //e, the RGB adapter on a //c. Its RGB monitor shows double "
         "hi-res without colour fringes.",
         "La Féline en slot 7 d'un ][+ ou //e, l'adaptateur RVB sur un //c. Son moniteur RVB affiche "
         "la double haute résolution sans franges de couleur.",
         {{"auto", "Automatic (DIX)", "Automatique (DIX)"}, {"disabled", "Absent", "Absente"},
          {"enabled", "Installed", "Installée"}}},

        // ---- Audio -----------------------------------------------------
        {"pom2_mockingboard", "audio", "Mockingboard (restart)", "Mockingboard (redémarrage)",
         "Two AY-3-8910 sound chips in stereo; the speech model adds an SSI263. On a //c it is the "
         "Mockingboard 4c.",
         "Deux puces son AY-3-8910 en stéréo ; le modèle à voix ajoute un SSI263. Sur un //c, c'est le "
         "Mockingboard 4c.",
         {{"ac", "Mockingboard A/C", "Mockingboard A/C"},
          {"sound2", "Mockingboard with speech (SSI263)", "Mockingboard avec voix (SSI263)"},
          {"disabled", "Absent", "Absent"}}},
        {"pom2_mockingboard_slot", "audio", "Mockingboard slot (restart)", "Slot du Mockingboard (redémarrage)",
         "Most programs look for it in slot 4. Automatic uses slot 2 when the mouse or DIX takes slot 4.",
         "La plupart des programmes le cherchent en slot 4. Automatique prend le slot 2 quand la souris "
         "ou DIX occupe le slot 4.",
         {{"auto", "Automatic", "Automatique"}, {"4", "Slot 4", "Slot 4"}, {"5", "Slot 5", "Slot 5"},
          {"2", "Slot 2", "Slot 2"}, {"1", "Slot 1", "Slot 1"}, {"7", "Slot 7", "Slot 7"}}},
        {"pom2_speaker_volume", "audio", "Speaker volume", "Volume du haut-parleur",
         "The Apple II's built-in speaker.", "Le haut-parleur intégré de l'Apple II.",
         percent(0, 200, 10, 100)},
        {"pom2_mockingboard_volume", "audio", "Mockingboard volume", "Volume du Mockingboard",
         nullptr, nullptr, percent(0, 200, 10, 100)},
        {"pom2_drive_sounds", "audio", "Disk drive sounds (restart)", "Bruits des lecteurs (redémarrage)",
         "The motor, head steps and door of the drives, from MAME's samples in system/pom2/floppy_samples. "
         "They are not in save states: keep them off with run-ahead.",
         "Le moteur, les pas de tête et la porte des lecteurs, d'après les échantillons de MAME dans "
         "system/pom2/floppy_samples. Ils ne sont pas dans les sauvegardes d'état : à désactiver avec le run-ahead.",
         {{"disabled", "Off", "Désactivés"}, {"enabled", "On", "Activés"}}},
        {"pom2_drive_sound_volume", "audio", "Disk drive sound volume", "Volume des bruits de lecteur",
         nullptr, nullptr, percent(0, 200, 10, 100)},

        // ---- Input -----------------------------------------------------
        {"pom2_paddle_deadzone", "input", "Analog dead zone", "Zone morte analogique",
         "Stick travel ignored around the centre.", "Course du stick ignorée autour du centre.",
         {{"12.5%", nullptr, nullptr}, {"0%", nullptr, nullptr}, {"5%", nullptr, nullptr},
          {"10%", nullptr, nullptr}, {"15%", nullptr, nullptr}, {"20%", nullptr, nullptr},
          {"25%", nullptr, nullptr}, {"30%", nullptr, nullptr}}},
        {"pom2_paddle_sensitivity", "input", "Analog sensitivity", "Sensibilité analogique",
         "Above 100% the paddle reaches its end before the stick does.",
         "Au-delà de 100 %, le paddle atteint sa butée avant le stick.",
         percent(50, 200, 25, 100)},
        {"pom2_joystick_axes", "input", "Joystick axes", "Axes du joystick", nullptr, nullptr,
         {{"normal", "Normal", "Normaux"}, {"invert-x", "Invert X", "X inversé"},
          {"invert-y", "Invert Y", "Y inversé"}, {"invert-both", "Invert X and Y", "X et Y inversés"},
          {"swap", "Swap X and Y", "X et Y échangés"}}},
        {"pom2_swap_buttons", "input", "Swap joystick buttons", "Échanger les boutons du joystick",
         "Exchange button 0 (B) and button 1 (A).", "Échange le bouton 0 (B) et le bouton 1 (A).",
         {{"disabled", "No", "Non"}, {"enabled", "Yes", "Oui"}}},
        {"pom2_alt_apple_keys", "input", "Alt keys are the Apple keys", "Touches Alt = touches Pomme",
         "Left Alt presses Open Apple and right Alt Solid Apple, which are also joystick buttons 0 and 1.",
         "Alt gauche appuie sur Pomme ouverte et Alt droite sur Pomme pleine, qui sont aussi les boutons 0 et 1.",
         {{"enabled", "Yes", "Oui"}, {"disabled", "No", "Non"}}},
        {"pom2_uppercase", "input", "Uppercase keyboard", "Clavier en majuscules",
         "The ][ and ][+ have no lowercase. Many //e games expect Caps Lock.",
         "Les ][ et ][+ n'ont pas de minuscules. Beaucoup de jeux //e attendent le verrouillage majuscule.",
         {{"auto", "Automatic (][ and ][+)", "Automatique (][ et ][+)"}, {"enabled", "Always", "Toujours"},
          {"disabled", "Never", "Jamais"}}},
        {"pom2_mouse", "input", "AppleMouse in slot 4 (restart)", "AppleMouse en slot 4 (redémarrage)",
         "Needs mouse_341-0270-c.bin in system/pom2. The //c has its mouse built in.",
         "Demande mouse_341-0270-c.bin dans system/pom2. Le //c a sa souris intégrée.",
         {{"disabled", "Absent", "Absente"}, {"enabled", "Installed", "Installée"}}},
        {"pom2_mouse_model", "input", "AppleMouse model (restart)", "Modèle d'AppleMouse (redémarrage)",
         "The card's microcontroller runs Apple's firmware when mouse_341-0269.bin is present, or "
         "AppleWin's model of it.",
         "Le microcontrôleur de la carte exécute le firmware d'Apple quand mouse_341-0269.bin est "
         "présent, ou le modèle d'AppleWin.",
         {{"auto", "Automatic", "Automatique"}, {"mcu", "Apple MCU firmware", "Firmware MCU d'Apple"},
          {"hle", "AppleWin model", "Modèle AppleWin"}}},
        {"pom2_mouse_speed", "input", "Mouse speed", "Vitesse de la souris", nullptr, nullptr,
         percent(25, 300, 25, 100)},

        // ---- Disks -----------------------------------------------------
        {"pom2_fast_disk_loading", "media", "Fast disk loading", "Chargement rapide des disques",
         "Run up to twelve frames per frontend frame while a floppy is read.",
         "Exécute jusqu'à douze trames par trame affichée pendant la lecture d'une disquette.",
         {{"enabled", "Enabled", "Activé"}, {"disabled", "Original drive timing", "Vitesse d'origine"}}},
        {"pom2_drive2", "media", "Disk II drive 2 (restart)", "Lecteur 2 du Disk II (redémarrage)",
         "Put the playlist's next 5.25-inch disk in drive 2 at start, for games that use two drives "
         "(Ultima, Wizardry, The Bard's Tale).",
         "Met le disque 5.25 suivant de la playlist dans le lecteur 2 au démarrage, pour les jeux à deux "
         "lecteurs (Ultima, Wizardry, The Bard's Tale).",
         {{"empty", "Empty", "Vide"}, {"next", "Next disk of the playlist", "Disque suivant de la playlist"}}},
        {"pom2_disk_control_drive", "media", "Disc Control drive", "Lecteur de Disc Control",
         "The drive that Quick Menu > Disc Control and the R3 button eject and load. Drive 2 takes 5.25-inch disks.",
         "Le lecteur qu'éjectent et chargent Menu rapide > Disc Control et le bouton R3. Le lecteur 2 prend "
         "les disques 5.25.",
         {{"1", "Drive 1", "Lecteur 1"}, {"2", "Drive 2", "Lecteur 2"}}},
        {"pom2_write_protect", "media", "Write-protect every disk", "Protéger tous les disques en écriture",
         "Disks always mount read-only and no private copy is made. Applies to the next disk inserted.",
         "Les disques sont montés en lecture seule, sans copie privée. S'applique au prochain disque inséré.",
         {{"disabled", "No", "Non"}, {"enabled", "Yes", "Oui"}}},
        {"pom2_autosave_interval", "media", "Disk save interval", "Intervalle de sauvegarde des disques",
         "How often guest writes reach the private disk copy.",
         "Fréquence à laquelle les écritures du programme atteignent la copie privée du disque.",
         {{"5", "5 seconds", "5 secondes"}, {"1", "1 second", "1 seconde"}, {"2", "2 seconds", "2 secondes"},
          {"10", "10 seconds", "10 secondes"}, {"30", "30 seconds", "30 secondes"}}},
        {"pom2_disk2_firmware", "media", "Disk II firmware (restart)", "Firmware Disk II (redémarrage)",
         "Apple's boot PROM and sequencer ROM when disk2.rom and diskii_p6.rom are present, or POM2's own.",
         "La PROM de démarrage et le séquenceur d'Apple si disk2.rom et diskii_p6.rom sont présents, "
         "sinon ceux de POM2.",
         {{"auto", "Apple's when present", "Celui d'Apple si présent"},
          {"pom2", "POM2's own", "Celui de POM2"}}},
    };
    return table;
}

// Each language's libretro structures, built once from the table.
struct Built {
    std::vector<retro_core_option_v2_category> categories;
    std::vector<retro_core_option_v2_definition> definitions;
    retro_core_options_v2 options{};
};

Built build(bool french)
{
    Built built;
    for (const auto& category : kCategories)
        built.categories.push_back({category.key, french ? category.fr : category.en,
                                    french ? category.infoFr : category.infoEn});
    built.categories.push_back({nullptr, nullptr, nullptr});
    for (const auto& option : options()) {
        retro_core_option_v2_definition definition{};
        definition.key = option.key;
        definition.desc = french ? option.fr : option.en;
        definition.info = french ? option.infoFr : option.infoEn;
        definition.category_key = option.category;
        std::size_t i = 0;
        for (const auto& choice : option.choices) {
            if (i + 1 >= RETRO_NUM_CORE_OPTION_VALUES_MAX) break;
            definition.values[i++] = {choice.value, french && choice.fr ? choice.fr : choice.en};
        }
        definition.values[i] = {nullptr, nullptr};
        definition.default_value = option.choices.front().value;
        built.definitions.push_back(definition);
    }
    built.definitions.push_back({});
    built.options = {built.categories.data(), built.definitions.data()};
    return built;
}

const Option* findOption(std::string_view key)
{
    for (const auto& option : options())
        if (key == option.key) return &option;
    return nullptr;
}

bool hasOptionVersion2 = false;

} // namespace

void registerCoreOptions(retro_environment_t environment)
{
    unsigned version = 0;
    hasOptionVersion2 = environment(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version) && version >= 2;
    if (hasOptionVersion2) {
        static Built english = build(false);
        static Built french = build(true);
        unsigned language = RETRO_LANGUAGE_ENGLISH;
        environment(RETRO_ENVIRONMENT_GET_LANGUAGE, &language);
        retro_core_options_v2_intl intl{&english.options,
                                        language == RETRO_LANGUAGE_FRENCH ? &french.options : nullptr};
        // The result says whether the frontend shows categories, not whether
        // it took the options: a v2 frontend always does.
        environment(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL, &intl);
        return;
    }

    // Version 0: "Description; default|value|value".
    static std::vector<std::string> text;
    static std::vector<retro_variable> variables;
    if (variables.empty()) {
        for (const auto& option : options()) {
            std::string entry = std::string(option.en) + "; ";
            for (std::size_t i = 0; i < option.choices.size(); ++i)
                entry += (i ? "|" : "") + std::string(option.choices[i].value);
            text.push_back(std::move(entry));
        }
        for (std::size_t i = 0; i < options().size(); ++i)
            variables.push_back({options()[i].key, text[i].c_str()});
        variables.push_back({nullptr, nullptr});
    }
    environment(RETRO_ENVIRONMENT_SET_VARIABLES, variables.data());
}

std::string_view coreOption(retro_environment_t environment, const char* key)
{
    retro_variable variable{key, nullptr};
    if (environment && environment(RETRO_ENVIRONMENT_GET_VARIABLE, &variable) && variable.value) {
        // A value the table does not list (an old configuration file) is the default.
        const auto* option = findOption(key);
        if (!option) return variable.value;
        for (const auto& choice : option->choices)
            if (std::strcmp(choice.value, variable.value) == 0) return choice.value;
    }
    const auto* option = findOption(key);
    return option ? std::string_view(option->choices.front().value) : std::string_view{};
}

bool updateOptionVisibility(retro_environment_t environment)
{
    if (!environment || !hasOptionVersion2) return false;
    static int lastMouse = -1;
    static int lastBoard = -1;
    static int lastDrive = -1;
    const bool mouse = coreOption(environment, "pom2_mouse") == "enabled"
                    || coreOption(environment, "pom2_machine").substr(0, 3) == "iic";
    const bool board = coreOption(environment, "pom2_mockingboard") != "disabled";
    const bool drive = coreOption(environment, "pom2_drive_sounds") == "enabled";
    if (int(mouse) == lastMouse && int(board) == lastBoard && int(drive) == lastDrive) return false;
    lastMouse = mouse;
    lastBoard = board;
    lastDrive = drive;
    const auto show = [&](const char* key, bool visible) {
        retro_core_option_display display{key, visible};
        environment(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &display);
    };
    show("pom2_mouse_model", coreOption(environment, "pom2_mouse") == "enabled");
    show("pom2_mouse_speed", mouse);
    show("pom2_mockingboard_slot", board);
    show("pom2_mockingboard_volume", board);
    show("pom2_drive_sound_volume", coreOption(environment, "pom2_drive_sounds") == "enabled");
    return true;
}

} // namespace pom2::libretro
