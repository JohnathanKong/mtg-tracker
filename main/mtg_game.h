#pragma once
#include <cstdint>

// Game-long counters are independent of desktop connectivity and saved locally.
// New games reset current values; deck library synchronization does NOT do this.
enum class MtgScreen : uint8_t { Dashboard, ChooseDeck, ConfirmReset };
enum class MtgCounter : uint8_t { Life, Poison, Rad, Damage, Casts, Deaths };

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
};

void mtg_game_start();
void mtg_game_reload_decks(); // Notify after verified profile sync
void mtg_game_view(MtgGameView &output);
