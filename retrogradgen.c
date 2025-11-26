#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH 7
#define HEIGHT 6

typedef struct {
    uint64_t position;
    uint64_t mask;
    int moves;
} Position;

typedef struct {
    uint64_t hash;
    uint8_t winning_col;
    uint8_t ply;
} CriticalEntry;

CriticalEntry *critical_positions;
size_t critical_count = 0;
size_t critical_capacity = 10000000;

int solve(Position *p, int alpha, int beta);
int is_winning_move(Position *p, int col);
int can_play(Position *p, int col);
void play_col(Position *p, int col);
void undo_col(Position *p, int col);
uint64_t compute_hash(Position *p);

int analyze_position(Position *p) {
    if (p->moves < 15 || p->moves > 28) 
        return -1; 
    int winning_moves[WIDTH];
    int win_count = 0;
    int drawing_moves = 0;
    
    for (int col = 0; col < WIDTH; col++) {
        if (!can_play(p, col)) continue;
        
        play_col(p, col);
        int score = -solve(p, -1, 1); 
        undo_col(p, col);
        
        if (score > 0) {
            winning_moves[win_count++] = col;
        } else if (score == 0) {
            drawing_moves++;
        }
    }
    if (win_count == 1) {
    
        if (!is_obvious_move(p, winning_moves[0])) {
            return winning_moves[0]; 
        }
    }
    
    return -1;
}

int is_obvious_move(Position *p, int col) {
    
    if (is_winning_move(p, col)) 
        return 1;
    
    Position opp = *p;
    opp.position ^= opp.mask; 
    if (is_winning_move(&opp, col))
        return 1;
    
    return 0;
}

void generate_all_positions(Position *p, int current_ply) {
    if (current_ply > 28) return; 
    
    int critical_col = analyze_position(p);
    if (critical_col >= 0) {
        
        if (critical_count >= critical_capacity) {
            critical_capacity *= 2;
            critical_positions = realloc(critical_positions, 
                critical_capacity * sizeof(CriticalEntry));
        }
        
        critical_positions[critical_count++] = (CriticalEntry){
            .hash = compute_hash(p),
            .winning_col = critical_col,
            .ply = current_ply
        };
        
        if (critical_count % 100000 == 0) {
            printf("Found %zu critical positions...\n", critical_count);
        }
    }
    
    for (int col = 0; col < WIDTH; col++) {
        if (!can_play(p, col)) continue;
        
        play_col(p, col);
        generate_all_positions(p, current_ply + 1);
        undo_col(p, col);
    }
}

int main() {
    critical_positions = malloc(critical_capacity * sizeof(CriticalEntry));
    
    printf("Generating critical positions database...\n");
    printf("This will take a while (hours)...\n\n");
    
    Position start = {0, 0, 0};
    generate_all_positions(&start, 0);
    
    printf("\nFound %zu critical positions!\n", critical_count);
    
    FILE *f = fopen("critical.db", "wb");
    
    uint32_t count = critical_count;
    fwrite(&count, sizeof(count), 1, f);
    
    size_t table_size = next_prime(critical_count * 2);
    uint32_t *keys = calloc(table_size, sizeof(uint32_t));
    uint8_t *vals = calloc(table_size, sizeof(uint8_t));
    
    for (size_t i = 0; i < critical_count; i++) {
        size_t idx = critical_positions[i].hash % table_size;
        while (keys[idx] != 0) {
            idx = (idx + 1) % table_size;
        }
        keys[idx] = (uint32_t)(critical_positions[i].hash >> 16);
        vals[idx] = critical_positions[i].winning_col;
    }
    
    uint32_t tsize = table_size;
    fwrite(&tsize, sizeof(tsize), 1, f);
    fwrite(keys, sizeof(uint32_t), table_size, f);
    fwrite(vals, sizeof(uint8_t), table_size, f);
    
    fclose(f);
    
    printf("Saved to critical.db (%zu bytes)\n", 
        sizeof(uint32_t) * 2 + table_size * 5);
    
    free(critical_positions);
    free(keys);
    free(vals);
    
    return 0;
}
