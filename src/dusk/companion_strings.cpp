// See companion_strings.h — LATIN-1 escapes, never raw UTF-8.
//
// These are best-effort translations produced alongside the code, not
// native-speaker copy. The plumbing is the durable part; individual words
// are cheap to correct here without touching any call site.
//
// Two rules when editing:
//   1. Non-ASCII must be a LATIN-1 escape, split if it precedes a hex digit:
//      "Esp\xE8" "ce", never a raw UTF-8 "Espèce" (renders as garbage and
//      mis-measures, since the font is single-byte).
//   2. Most rows are drawn literally — a stray '%' becomes a printf directive
//      and reads a non-existent argument. Only STR_COMBO_ON, STR_SLOT_CLEARED,
//      STR_SLOT_BOUND and STR_EQUIPPED take a "%s" (the item/slot name); every
//      other row must contain no '%'.

#include "dusk/companion_strings.h"

#include "dolphin/os/OSRtc.h"

namespace dusk::companion {
namespace {

// [id][language], indexed by OS_LANGUAGE_* (0=EN 1=DE 2=FR 3=ES 4=IT).
const char* const l_strings[STR_COUNT][5] = {
    // STR_RESET
    {"Reset",
     "Zur\xFC" "cksetzen",
     "R\xE9initialiser",
     "Reiniciar",
     "Reimposta"},
    // STR_SPECIES
    {"Species",
     "Art",
     "Esp\xE8" "ce",
     "Especie",
     "Specie"},
    // STR_SCENT
    {"Scent",
     "Geruch",
     "Odeur",
     "Olor",
     "Odore"},
    // STR_WARP
    {"Warp",
     "Warpen",
     "Portails",
     "Portales",
     "Portali"},
    // STR_NO_MAP
    {"No map for this area",
     "Keine Karte f\xFCr dieses Gebiet",
     "Aucune carte pour cette zone",
     "No hay mapa de esta zona",
     "Nessuna mappa per quest'area"},
    // STR_LOADING_MAP
    {"Loading map...",
     "Karte wird geladen...",
     "Chargement de la carte...",
     "Cargando mapa...",
     "Caricamento mappa..."},
    // STR_LOADING_FLOOR
    {"Loading floor map...",
     "Etagenkarte wird geladen...",
     "Chargement du plan...",
     "Cargando plano...",
     "Caricamento piano..."},
    // STR_MAP_ON_MAIN
    {"View Map on Main Screen",
     "Karte auf dem Hauptbildschirm",
     "Carte sur l'\xE9" "cran principal",
     "Mapa en la pantalla principal",
     "Mappa sullo schermo principale"},
    // STR_DRAG_EQUIP
    {"Drag an item onto X or Y to equip",
     "Item auf X oder Y ziehen zum Ausr\xFCsten",
     "Faites glisser un objet sur X ou Y",
     "Arrastra un objeto a X o Y",
     "Trascina un oggetto su X o Y"},
    // STR_COMBINE_BOW
    {"Combine with Bow?",
     "Mit dem Bogen kombinieren?",
     "Combiner avec l'arc ?",
     "\xBF" "Combinar con el arco?",
     "Combinare con l'arco?"},
    // STR_BOMB_ARROWS
    {"Bomb Arrows",
     "Bombenpfeile",
     "Fl\xE8" "ches-bombes",
     "Flechas bomba",
     "Frecce bomba"},
    // STR_REPLACE
    {"Replace",
     "Ersetzen",
     "Remplacer",
     "Sustituir",
     "Sostituisci"},
    // STR_NO_LETTERS
    {"No letters yet.",
     "Noch keine Briefe.",
     "Aucune lettre.",
     "No hay cartas.",
     "Nessuna lettera."},
    // STR_COMBO_ON  (%s = the localized item name; English wording unchanged)
    {"%s + Bow combo!",
     "%s + Bogen-Kombi!",
     "Combo %s + arc !",
     "Combo %s + arco!",
     "Combo %s + arco!"},
    // STR_NO_EQUIP_MENU
    {"Can't equip while a menu is open",
     "Nicht m\xF6glich, solange ein Men\xFC offen ist",
     "\xC9quipement impossible: menu ouvert",
     "No puedes equipar con un men\xFA abierto",
     "Non puoi equipaggiare con un menu aperto"},
    // STR_NO_EQUIP_WOLF
    {"Can't equip items as a wolf",
     "Als Wolf nicht ausr\xFCstbar",
     "\xC9quipement impossible en loup",
     "No puedes equipar como lobo",
     "Non puoi equipaggiare da lupo"},
    // STR_NO_SLOT
    {"Can't put that on a slot",
     "Das passt in keinen Slot",
     "Impossible de placer ceci",
     "Eso no va en una ranura",
     "Non puoi metterlo in uno slot"},
    // STR_SLOT_CLEARED
    {"Cleared slot %s",
     "Slot %s geleert",
     "Emplacement %s vid\xE9",
     "Ranura %s vaciada",
     "Slot %s svuotato"},
    // STR_SLOT_BOUND
    {"Bound to slot %s",
     "Auf Slot %s gelegt",
     "Assign\xE9 \xE0 l'emplacement %s",
     "Asignado a la ranura %s",
     "Assegnato allo slot %s"},
    // STR_NO_GEAR_MENU
    {"Can't change gear while a menu is open",
     "Ausr\xFCstungswechsel bei offenem Men\xFC nicht m\xF6glich",
     "Changement d'\xE9quipement impossible: menu ouvert",
     "No puedes cambiar equipo con un men\xFA abierto",
     "Non puoi cambiare equipaggiamento"},
    // STR_NO_GEAR_WOLF
    {"Can't change gear as a wolf",
     "Als Wolf kein Ausr\xFCstungswechsel",
     "Changement d'\xE9quipement impossible en loup",
     "No puedes cambiar equipo como lobo",
     "Non puoi cambiare da lupo"},
    // STR_EQUIPPED
    {"Equipped %s",
     "%s ausger\xFCstet",
     "%s \xE9quip\xE9",
     "%s equipado",
     "%s equipaggiato"},
    // STR_GEAR
    {"gear",
     "Ausr\xFCstung",
     "\xE9quipement",
     "equipo",
     "equipaggiamento"},
    // STR_NO_WARP_HERE
    {"Can't warp from here",
     "Von hier ist kein Warp m\xF6glich",
     "T\xE9l\xE9portation impossible ici",
     "No puedes teletransportarte aqu\xED",
     "Non puoi teletrasportarti qui"},
};

}  // namespace

const char* txt(StringId id) {
    // No `id < 0`: StringId has no negative enumerators, so the compiler picks
    // an unsigned underlying type and that test is always false.
    if (id >= STR_COUNT) {
        return "";
    }
    int lang = (int)OSGetLanguage();
    if (lang < 0 || lang > 4) {
        lang = 0;
    }
    const char* s = l_strings[id][lang];
    if (s == nullptr || s[0] == 0) {
        s = l_strings[id][0];
    }
    // A row added to the enum without a table row is silently zero-filled by
    // C++, not a compile error — and several of these are used as printf
    // formats, so a null would crash rather than misdraw.
    return s != nullptr ? s : "";
}

}  // namespace dusk::companion
