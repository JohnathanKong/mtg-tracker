#pragma once
#include <cstdint>

// Life, commander casts and derived next-cast tax are permanently visible.
// The four tile slots are populated from the deck's selected trackers;
// horizontal swipes move between groups of four without hiding Life/Casts/Tax.
enum class MtgScreen : uint8_t { Dashboard, ChooseDeck, ConfirmReset, EditCounters };
enum class MtgCounter : uint8_t { Life, Poison, Rad, Damage, Casts, Deaths, Dynamic, CommanderDamage };

struct MtgTile {
    char label[36]{};
    int value = 0;
    uint8_t accent = 0;
    bool selected = false;
};
struct MtgMenuChoice {
    char label[36]{};
    bool checked = false;
};

struct MtgGameView {
    MtgScreen screen = MtgScreen::Dashboard;
    MtgCounter selected = MtgCounter::Life;
    int life = 40, poison = 0, rad = 0, damage = 0, casts = 0, deaths = 0;
    int deckIndex = 0, deckCount = 0;
    char deckName[64]{};
    char commander[64]{};
    char deckId[96]{};
    bool hasArtwork = false;
    uint32_t artworkRevision = 0;

    int page = 0, pageCount = 1;
    int tileCount = 0;
    MtgTile tiles[4]{};
    // Touch-controlled horizontal carousel. The offset is in physical pixels
    // and updates while the finger moves, not just when it is released.
    bool dragActive = false;
    int dragOffsetPx = 0;
    int dragTargetPage = -1;
    int dragTileCount = 0;
    MtgTile dragTiles[2]{};
    int damagePlayers = 3;
    int damagePage = 0;
    int damagePageCount = 1;
    int damageValues[3]{};
    int damageIndices[3]{};
    int selectedDamage = -1;
    int addPage = 0, addPageCount = 1;
    int selectedTrackerCount = 0;
    int choiceCount = 0;
    MtgMenuChoice choices[4]{};
};

void mtg_game_start();
void mtg_game_reload_decks(); // Notify after verified profile and artwork sync
void mtg_game_view(MtgGameView &output);
