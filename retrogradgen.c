/*
 * RETROGRADE ANALYSIS GENERATOR
 * =============================
 * Generates a "critical positions database" for Connect 4
 * 
 * A position is CRITICAL if:
 *   - Exactly ONE move wins
 *   - All other moves lose or draw
 *   - The winning move is NOT obvious (not win-in-1 or forced block)
 *
 * Usage:
 *   gcc -O3 -o generator retrograde_generator.c -lpthread
 *   ./generator
 *
 * Output: critical.db (~5-10MB)
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
/* ============== CONFIGURATION ============== */
#define WIDTH   7
#define HEIGHT  6
/* Which plies to analyze (Pascal's book covers 0-14) */
#define MIN_PLY 15
#define MAX_PLY 28
/* Solver transposition table size (2^23 = 8M entries) */
#define TT_SIZE (1 << 23)
#define TT_MASK (TT_SIZE - 1)
/* Score bounds */
#define MIN_SCORE (-(WIDTH * HEIGHT) / 2 + 3)
#define MAX_SCORE ((WIDTH * HEIGHT + 1) / 2 - 3)
/* ============== BITBOARD CONSTANTS ============== */
static uint64_t bottom_mask_col[WIDTH];
static uint64_t column_mask_col[WIDTH];
static uint64_t bottom_mask = 0;
static uint64_t board_mask = 0;
/* Center-first column order for better pruning */
static const int column_order[WIDTH] = {3, 2, 4, 1, 5, 0, 6};
/* ============== POSITION STRUCTURE ============== */
typedef struct {
    uint64_t current;  /* Stones of player to move */
    uint64_t mask;     /* All stones on board */
    int ply;           /* Number of moves played */
} Position;
/* ============== TRANSPOSITION TABLE ============== */
typedef struct {
    uint64_t key;
    int8_t value;
} TTEntry;
static TTEntry *tt = NULL;
/* ============== CRITICAL POSITIONS STORAGE ============== */
typedef struct {
    uint64_t hash;
    uint8_t winning_col;
} CriticalEntry;
static CriticalEntry *critical_list = NULL;
static size_t critical_count = 0;
static size_t critical_capacity = 0;
/* Statistics */
static uint64_t positions_analyzed = 0;
static uint64_t positions_critical = 0;
static uint64_t positions_skipped = 0;
/* ============== UTILITY FUNCTIONS ============== */
/* Find next prime >= n (for hash table sizing) */
static bool is_prime(size_t n) {
    if (n < 2) return false;
    if (n == 2) return true;
    if (n % 2 == 0) return false;
    for (size_t i = 3; i * i <= n; i += 2) {
        if (n % i == 0) return false;
    }
    return true;
}
static size_t next_prime(size_t n) {
    while (!is_prime(n)) n++;
    return n;
}
/* ============== BITBOARD HELPERS ============== */
static void init_bitboards(void) {
    bottom_mask = 0;
    board_mask = 0;
    for (int col = 0; col < WIDTH; col++) {
        bottom_mask_col[col] = 1ULL << (col * (HEIGHT + 1));
        column_mask_col[col] = ((1ULL << HEIGHT) - 1) << (col * (HEIGHT + 1));
        bottom_mask |= bottom_mask_col[col];
        board_mask |= column_mask_col[col];
    }
}
static inline uint64_t top_mask_col(int col) {
    return 1ULL << ((HEIGHT - 1) + col * (HEIGHT + 1));
}
static inline bool can_play(const Position *p, int col) {
    return (p->mask & top_mask_col(col)) == 0;
}
static inline uint64_t move_bit(const Position *p, int col) {
    return (p->mask + bottom_mask_col[col]) & column_mask_col[col];
}
static inline void play_col(Position *p, int col) {
    uint64_t move = move_bit(p, col);
    p->current ^= p->mask;
    p->mask |= move;
    p->ply++;
}
static inline void undo_col(Position *p, int col) {
    p->ply--;
    /* Find the top stone in this column and remove it */
    uint64_t col_stones = p->mask & column_mask_col[col];
    uint64_t top_stone = 0;
    for (int row = HEIGHT - 1; row >= 0; row--) {
        uint64_t bit = 1ULL << (row + col * (HEIGHT + 1));
        if (col_stones & bit) {
            top_stone = bit;
            break;
        }
    }
    p->mask ^= top_stone;
    p->current ^= p->mask;
}
/* Compute all winning positions for a given player bitboard */
static uint64_t compute_winning_positions(uint64_t position, uint64_t mask) {
    uint64_t r = 0;
    uint64_t p;
    
    /* Vertical (need 3 in a column, 4th on top) */
    r = (position << 1) & (position << 2) & (position << 3);
    
    /* Horizontal */
    p = (position << (HEIGHT + 1)) & (position << 2 * (HEIGHT + 1));
    r |= p & (position << 3 * (HEIGHT + 1));
    r |= p & (position >> (HEIGHT + 1));
    p = (position >> (HEIGHT + 1)) & (position >> 2 * (HEIGHT + 1));
    r |= p & (position << (HEIGHT + 1));
    r |= p & (position >> 3 * (HEIGHT + 1));
    
    /* Diagonal / */
    p = (position << HEIGHT) & (position << 2 * HEIGHT);
    r |= p & (position << 3 * HEIGHT);
    r |= p & (position >> HEIGHT);
    p = (position >> HEIGHT) & (position >> 2 * HEIGHT);
    r |= p & (position << HEIGHT);
    r |= p & (position >> 3 * HEIGHT);
    
    /* Diagonal \ */
    p = (position << (HEIGHT + 2)) & (position << 2 * (HEIGHT + 2));
    r |= p & (position << 3 * (HEIGHT + 2));
    r |= p & (position >> (HEIGHT + 2));
    p = (position >> (HEIGHT + 2)) & (position >> 2 * (HEIGHT + 2));
    r |= p & (position << (HEIGHT + 2));
    r |= p & (position >> 3 * (HEIGHT + 2));
    
    return r & (board_mask ^ mask);
}
/* Check if current player can win immediately */
static inline bool can_win_next(const Position *p) {
    uint64_t winning = compute_winning_positions(p->current, p->mask);
    uint64_t possible = (p->mask + bottom_mask) & board_mask;
    return (winning & possible) != 0;
}
/* Get winning column if win-in-1 exists, else -1 */
static int get_winning_col(const Position *p) {
    uint64_t winning = compute_winning_positions(p->current, p->mask);
    uint64_t possible = (p->mask + bottom_mask) & board_mask;
    uint64_t winning_moves = winning & possible;
    
    if (winning_moves == 0) return -1;
    
    for (int col = 0; col < WIDTH; col++) {
        if (winning_moves & column_mask_col[col]) {
            return col;
        }
    }
    return -1;
}
/* Get opponent's winning positions */
static inline uint64_t opponent_winning_positions(const Position *p) {
    return compute_winning_positions(p->current ^ p->mask, p->mask);
}
/* Get possible moves that don't immediately lose */
static uint64_t non_losing_moves(const Position *p) {
    uint64_t possible = (p->mask + bottom_mask) & board_mask;
    uint64_t opponent_wins = opponent_winning_positions(p);
    uint64_t forced = possible & opponent_wins;
    
    if (forced) {
        /* Must block. If multiple threats, we lose anyway */
        if (forced & (forced - 1)) return 0;  /* Multiple threats */
        possible = forced;
    }
    
    /* Avoid moves that create a threat for opponent above */
    return possible & ~(opponent_wins >> 1);
}
/* Unique key for position */
static inline uint64_t position_key(const Position *p) {
    return p->current + p->mask;
}
/* Score a move by counting threats it creates */
static int move_score(const Position *p, uint64_t move) {
    uint64_t new_pos = p->current | move;
    uint64_t threats = compute_winning_positions(new_pos, p->mask);
    return __builtin_popcountll(threats);
}
/* ============== TRANSPOSITION TABLE ============== */
static void tt_init(void) {
    tt = (TTEntry *)calloc(TT_SIZE, sizeof(TTEntry));
    if (!tt) {
        fprintf(stderr, "Failed to allocate transposition table!\n");
        exit(1);
    }
}
static void tt_clear(void) {
    memset(tt, 0, TT_SIZE * sizeof(TTEntry));
}
static void tt_free(void) {
    free(tt);
    tt = NULL;
}
static inline void tt_store(uint64_t key, int value) {
    size_t idx = key & TT_MASK;
    tt[idx].key = key;
    tt[idx].value = (int8_t)(value - MIN_SCORE + 1);
}
static inline int tt_probe(uint64_t key, int *value) {
    size_t idx = key & TT_MASK;
    if (tt[idx].key == key && tt[idx].value != 0) {
        *value = tt[idx].value + MIN_SCORE - 1;
        return 1;
    }
    return 0;
}
/* ============== SOLVER (NEGAMAX) ============== */
static int negamax(Position *p, int alpha, int beta) {
    /* Check for immediate win */
    if (can_win_next(p)) {
        return (WIDTH * HEIGHT + 1 - p->ply) / 2;
    }
    
    /* Get non-losing moves */
    uint64_t possible = non_losing_moves(p);
    if (possible == 0) {
        return -(WIDTH * HEIGHT - p->ply) / 2;
    }
    
    /* Draw if board nearly full */
    if (p->ply >= WIDTH * HEIGHT - 2) {
        return 0;
    }
    
    /* Tighten bounds */
    int min = -(WIDTH * HEIGHT - 2 - p->ply) / 2;
    if (alpha < min) {
        alpha = min;
        if (alpha >= beta) return alpha;
    }
    
    int max = (WIDTH * HEIGHT - 1 - p->ply) / 2;
    if (beta > max) {
        beta = max;
        if (alpha >= beta) return beta;
    }
    
    /* Transposition table lookup */
    uint64_t key = position_key(p);
    int tt_val;
    if (tt_probe(key, &tt_val)) {
        if (tt_val >= beta) return tt_val;
        if (tt_val <= alpha) return tt_val;
    }
    
    /* Move ordering: sort by threat count */
    typedef struct { uint64_t move; int score; } MoveEntry;
    MoveEntry moves[WIDTH];
    int num_moves = 0;
    
    for (int i = 0; i < WIDTH; i++) {
        int col = column_order[i];
        uint64_t move = possible & column_mask_col[col];
        if (move) {
            moves[num_moves].move = move;
            moves[num_moves].score = move_score(p, move);
            num_moves++;
        }
    }
    
    /* Sort descending by score */
    for (int i = 0; i < num_moves - 1; i++) {
        for (int j = i + 1; j < num_moves; j++) {
            if (moves[j].score > moves[i].score) {
                MoveEntry tmp = moves[i];
                moves[i] = moves[j];
                moves[j] = tmp;
            }
        }
    }
    
    /* Search */
    int best = -WIDTH * HEIGHT;
    for (int i = 0; i < num_moves; i++) {
        Position child = *p;
        child.current ^= child.mask;
        child.mask |= moves[i].move;
        child.ply++;
        
        int score = -negamax(&child, -beta, -alpha);
        
        if (score > best) best = score;
        if (score > alpha) alpha = score;
        if (alpha >= beta) break;
    }
    
    /* Store in TT */
    tt_store(key, best);
    
    return best;
}
/* Solve position: returns score (positive = win, negative = loss, 0 = draw) */
static int solve(Position *p) {
    if (can_win_next(p)) {
        return (WIDTH * HEIGHT + 1 - p->ply) / 2;
    }
    
    int min = -(WIDTH * HEIGHT - p->ply) / 2;
    int max = (WIDTH * HEIGHT + 1 - p->ply) / 2;
    
    while (min < max) {
        int med = min + (max - min) / 2;
        if (med <= 0 && min / 2 < med) med = min / 2;
        else if (med >= 0 && max / 2 > med) med = max / 2;
        
        int r = negamax(p, med, med + 1);
        
        if (r <= med) max = r;
        else min = r;
    }
    
    return min;
}
/* ============== CRITICAL POSITION DETECTION ============== */
/* Check if a move is "obvious" (win-in-1 or forced block) */
static bool is_obvious_move(const Position *p, int col) {
    /* Check if this move wins immediately */
    uint64_t my_winning = compute_winning_positions(p->current, p->mask);
    uint64_t move = move_bit(p, col);
    if (my_winning & move) {
        return true;  /* Win-in-1, obvious */
    }
    
    /* Check if opponent threatens here (forced block) */
    uint64_t opp_winning = opponent_winning_positions(p);
    if (opp_winning & move) {
        return true;  /* Must block, obvious */
    }
    
    return false;
}
/* Add a critical position to our list */
static void add_critical(uint64_t hash, int winning_col) {
    if (critical_count >= critical_capacity) {
        critical_capacity = critical_capacity ? critical_capacity * 2 : 1000000;
        critical_list = (CriticalEntry *)realloc(critical_list, 
            critical_capacity * sizeof(CriticalEntry));
        if (!critical_list) {
            fprintf(stderr, "Failed to allocate critical list!\n");
            exit(1);
        }
    }
    
    critical_list[critical_count].hash = hash;
    critical_list[critical_count].winning_col = (uint8_t)winning_col;
    critical_count++;
    positions_critical++;
}
/* Analyze a position: returns winning col if critical, -1 otherwise */
static int analyze_position(Position *p) {
    positions_analyzed++;
    
    /* Skip if outside our target ply range */
    if (p->ply < MIN_PLY || p->ply > MAX_PLY) {
        positions_skipped++;
        return -1;
    }
    
    /* Skip if there's a win-in-1 (trivial) */
    if (can_win_next(p)) {
        positions_skipped++;
        return -1;
    }
    
    /* Skip if no non-losing moves (lost position) */
    uint64_t possible = non_losing_moves(p);
    if (possible == 0) {
        positions_skipped++;
        return -1;
    }
    
    /* Evaluate each move */
    int winning_cols[WIDTH];
    int win_count = 0;
    int draw_count = 0;
    int loss_count = 0;
    
    for (int col = 0; col < WIDTH; col++) {
        if (!can_play(p, col)) continue;
        if (!(possible & column_mask_col[col])) continue;
        
        Position child = *p;
        play_col(&child, col);
        
        int score = -solve(&child);
        
        if (score > 0) {
            winning_cols[win_count++] = col;
        } else if (score == 0) {
            draw_count++;
        } else {
            loss_count++;
        }
    }
    
    /* CRITICAL: exactly 1 winning move exists */
    if (win_count == 1) {
        int winning_col = winning_cols[0];
        
        /* But only if it's NOT obvious */
        if (!is_obvious_move(p, winning_col)) {
            return winning_col;
        }
    }
    
    positions_skipped++;
    return -1;
}
/* ============== POSITION GENERATION ============== */
static void generate_positions(Position *p, int depth);
/* Progress tracking */
static int last_progress = -1;
static time_t start_time;
static void print_progress(int current_ply0_col) {
    int progress = (current_ply0_col * 100) / WIDTH;
    if (progress != last_progress) {
        last_progress = progress;
        time_t now = time(NULL);
        int elapsed = (int)(now - start_time);
        printf("\rProgress: %d%% | Analyzed: %llu | Critical: %llu | Time: %dm %ds    ",
            progress, 
            (unsigned long long)positions_analyzed,
            (unsigned long long)positions_critical,
            elapsed / 60, elapsed % 60);
        fflush(stdout);
    }
}
/* Recursive position generator */
static void generate_positions(Position *p, int depth) {
    /* Analyze this position if in range */
    if (p->ply >= MIN_PLY && p->ply <= MAX_PLY) {
        int critical_col = analyze_position(p);
        if (critical_col >= 0) {
            uint64_t hash = position_key(p);
            add_critical(hash, critical_col);
        }
    }
    
    /* Stop recursion if deep enough */
    if (p->ply >= MAX_PLY) {
        return;
    }
    
    /* Check for game over */
    if (can_win_next(p)) {
        return;  /* Game would end */
    }
    
    /* Recurse into children */
    for (int col = 0; col < WIDTH; col++) {
        if (!can_play(p, col)) continue;
        
        /* Progress update at ply 0 */
        if (p->ply == 0) {
            print_progress(col);
        }
        
        Position child = *p;
        play_col(&child, col);
        
        /* Skip if this move won (game over) */
        uint64_t prev_player = child.current ^ child.mask;
        uint64_t winning = compute_winning_positions(prev_player, child.mask ^ 
            ((child.mask + bottom_mask_col[col]) & column_mask_col[col]));
        /* Actually, just check if previous player won */
        
        generate_positions(&child, depth + 1);
    }
}
/* ============== SAVE DATABASE ============== */
static void save_database(const char *filename) {
    printf("\n\nSaving %zu critical positions to %s...\n", critical_count, filename);
    
    if (critical_count == 0) {
        printf("No critical positions found!\n");
        return;
    }
    
    /* Build hash table */
    size_t table_size = next_prime(critical_count * 2);
    uint32_t *keys = (uint32_t *)calloc(table_size, sizeof(uint32_t));
    uint8_t *values = (uint8_t *)calloc(table_size, sizeof(uint8_t));
    
    if (!keys || !values) {
        fprintf(stderr, "Failed to allocate hash table!\n");
        free(keys);
        free(values);
        return;
    }
    
    /* Insert all critical positions */
    size_t collisions = 0;
    for (size_t i = 0; i < critical_count; i++) {
        uint64_t hash = critical_list[i].hash;
        uint32_t partial_key = (uint32_t)(hash >> 16);
        size_t idx = hash % table_size;
        
        /* Linear probing */
        while (keys[idx] != 0) {
            idx = (idx + 1) % table_size;
            collisions++;
        }
        
        keys[idx] = partial_key;
        values[idx] = critical_list[i].winning_col;
    }
    
    printf("Hash table: %zu entries, %zu collisions\n", table_size, collisions);
    
    /* Write to file */
    FILE *f = fopen(filename, "wb");
    if (!f) {
        fprintf(stderr, "Failed to open %s for writing!\n", filename);
        free(keys);
        free(values);
        return;
    }
    
    /* Header */
    uint8_t header[8];
    header[0] = WIDTH;
    header[1] = HEIGHT;
    header[2] = MIN_PLY;
    header[3] = MAX_PLY;
    header[4] = 4;  /* key_bytes */
    header[5] = 1;  /* value_bytes */
    header[6] = 0;  /* reserved */
    header[7] = 0;  /* reserved */
    fwrite(header, 1, 8, f);
    
    /* Table size */
    uint32_t tsize = (uint32_t)table_size;
    fwrite(&tsize, sizeof(tsize), 1, f);
    
    /* Data */
    fwrite(keys, sizeof(uint32_t), table_size, f);
    fwrite(values, sizeof(uint8_t), table_size, f);
    
    fclose(f);
    
    /* Report size */
    size_t file_size = 8 + 4 + table_size * 5;
    printf("Saved! File size: %.2f MB\n", file_size / (1024.0 * 1024.0));
    
    free(keys);
    free(values);
}
/* ============== MAIN ============== */
int main(int argc, char *argv[]) {
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║     CONNECT 4 CRITICAL POSITION DATABASE GENERATOR       ║\n");
    printf("╠══════════════════════════════════════════════════════════╣\n");
    printf("║  Analyzing positions from ply %2d to %2d                   ║\n", MIN_PLY, MAX_PLY);
    printf("║  This may take several hours...                          ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");
    
    /* Initialize */
    init_bitboards();
    tt_init();
    
    start_time = time(NULL);
    
    /* Generate all positions and find critical ones */
    Position start = {0, 0, 0};
    generate_positions(&start, 0);
    
    /* Summary */
    time_t end_time = time(NULL);
    int total_time = (int)(end_time - start_time);
    
    printf("\n\n");
    printf("════════════════════════════════════════════════════════════\n");
    printf("                        SUMMARY                             \n");
    printf("════════════════════════════════════════════════════════════\n");
    printf("  Positions analyzed:  %llu\n", (unsigned long long)positions_analyzed);
    printf("  Critical found:      %llu\n", (unsigned long long)positions_critical);
    printf("  Skipped (trivial):   %llu\n", (unsigned long long)positions_skipped);
    printf("  Total time:          %d min %d sec\n", total_time / 60, total_time % 60);
    printf("════════════════════════════════════════════════════════════\n\n");
    
    /* Save database */
    save_database("critical.db");
    
    /* Cleanup */
    tt_free();
    free(critical_list);
    
    printf("\nDone! Use critical.db with your bot.\n");
    
    return 0;
}
