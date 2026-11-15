#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "data.h"
#include "item.h"
#include "pokemon.h"
#include "mod_manager.h"
#include "wild_encounter.h"

static const struct WildPokemonInfo sFixtureLandInfo = { .encounterRate = 42 };
static int sFailNextMalloc;

void *__real_malloc(size_t size);

void *__wrap_malloc(size_t size)
{
    if (sFailNextMalloc)
    {
        sFailNextMalloc = 0;
        return NULL;
    }
    return __real_malloc(size);
}

const struct WildPokemonHeader gWildMonHeaders_ROM[] =
{
    { .mapGroup = 1, .mapNum = 2, .landMonsInfo = &sFixtureLandInfo },
    { .mapGroup = 0xFF },
};

const u8 gSpeciesNames_ROM[NUM_SPECIES][POKEMON_NAME_LENGTH + 1] =
{
    [1] = { [0] = 0xA5 },
};

const struct SpeciesInfo gSpeciesInfo_ROM[NUM_SPECIES] =
{
    [1] = { .baseHP = 77 },
};

const struct BattleMove gBattleMoves_ROM[MOVES_COUNT] =
{
    [1] = { .power = 42 },
};

const struct Item gItems_ROM[ITEMS_COUNT] =
{
    [1] = { .price = 333 },
};

int main(void)
{
    assert(gWildMonHeaders == NULL);
    sFailNextMalloc = 1;
    assert(!ModManager_Init());
    assert(gWildMonHeaders == NULL);
    assert(gSpeciesNames[1][0] == 0);
    assert(gSpeciesInfo[1].baseHP == 0);

    assert(ModManager_Init());
    assert(gWildMonHeaders != NULL);
    assert(gWildMonHeaders != gWildMonHeaders_ROM);
    assert(gWildMonHeaders[0].mapGroup == 1);
    assert(gWildMonHeaders[0].mapNum == 2);
    assert(gWildMonHeaders[0].landMonsInfo == &sFixtureLandInfo);
    assert(gWildMonHeaders[1].mapGroup == 0xFF);
    assert(memcmp(gSpeciesNames, gSpeciesNames_ROM, sizeof(gSpeciesNames_ROM)) == 0);
    assert(memcmp(gSpeciesInfo, gSpeciesInfo_ROM, sizeof(gSpeciesInfo)) == 0);
    assert(memcmp(gBattleMoves, gBattleMoves_ROM, sizeof(gBattleMoves_ROM)) == 0);
    assert(memcmp(gItems, gItems_ROM, sizeof(gItems)) == 0);

    struct WildPokemonHeader *initializedHeaders = gWildMonHeaders;
    assert(ModManager_Init());
    assert(gWildMonHeaders == initializedHeaders);
    return 0;
}
