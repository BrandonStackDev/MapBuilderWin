#include "raylib.h"
#include "raymath.h"

//#define STB_PERLIN_IMPLEMENTATION
#include "stb_perlin.h"

#include "models.h"
#include <math.h>
#include <stdio.h>
#include <float.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/** FOR CREATING DIRECTORYS */
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
    #include <direct.h>
    #define MKDIR(path) _mkdir(path)
#else
    #include <unistd.h>
    #define MKDIR(path) mkdir(path, 0755)
#endif

void EnsureDirectoryExists(const char *path) {
    struct stat st = {0};
    if (stat(path, &st) == -1) {
        MKDIR(path);
    }
}
//********************************** */
#define CHUNK_SIZE 64
#define MAP_SIZE 1024
#define CHUNK_COUNT (MAP_SIZE / CHUNK_SIZE)
#define MAP_SIZE_SCALE 0.5f
#define HEIGHT_SCALE 60.0f
#define MAP_SCALE 16
#define UPSCALED_TEXTURE_SIZE 1024

#define ROAD_MAP_SIZE 1024
#define NUM_FEATURE_POINTS 512
#define FEATURE_SPACING 64  // test with 16, 32, or 64 pixels
#define FLATTEN_RADIUS 2
#define FLATTEN_STRENGTH 0.0034f //james bond, 0.007f is a bit strong with the new flattening, but creates really nice ridges.
#define FLATTEN_LERP_FACT 0.42f

//fpr trees/props
#define ASSET_TWEAK_MOD 4500
#define ASSET_TWEAK_THRESH 25 //tweak MOD and THRESH values together for density of tree placement

//erosion defines
#define EROSION_DROPLETS     100000
#define EROSION_LIFETIME     50
#define INERTIA              0.05f
#define GRAVITY              9.81f
#define EVAPORATE_SPEED      0.004f
#define CAPACITY_MULTIPLIER  4.0f
#define DEPOSITION_SPEED     0.1f
#define EROSION_SPEED        0.3f
#define MIN_HEIGHT           0.0f

//for baking cookies
#define TILE_GRID_SIZE 8 //sync with preview.c
#define TILE_SIZE (CHUNK_SIZE / TILE_GRID_SIZE)
#define CHUNK_WORLD_SIZE 1024.0f
#define TILE_WORLD_SIZE (CHUNK_WORLD_SIZE / TILE_GRID_SIZE)

//WATER
#define MAX_WATER_TILES 1024 * 128
#define MAX_WATER_FEATURES 128
#define MAX_WATER_PATCHES_PER_FEATURE 128
#define WATER_PATCH_SIZE 64
#define WATER_HEIGHT 0.2f
float sealevel = 0.42f; // dont want to create and hide too much water, wasted polygons under the map, actually its easier to just set this really high I think

typedef struct {
    int x, y;
} Int2;

typedef struct {
    Int2 start;
    Int2 end;
} WaterPatch;

typedef struct {
    WaterPatch patches[MAX_WATER_PATCHES_PER_FEATURE];
    int patchCount;
} WaterFeature;


//cool inline
#define MakeTileFolderPath(buf, cx, cy, tx, ty) \
    snprintf(buf, sizeof(buf), "map/chunk_%02d_%02d/tile_64/%02d_%02d/", cx, cy, tx, ty)
//max value (there is probably an easier way to do this but chatgpt gave me this cool code so I thought I would use it)
#define MAX(a, b) ((a) > (b) ? (a) : (b))

//models we use for tile batching (all static props)
//Model tree, treeBg, rock; -> static prop models handled mostly in models.h
// Example object type
typedef struct EnvObject {
    Model model;
    Vector3 position;
    Matrix transform;
    //int type;
    //bool pointEntity;
} EnvObject;

/// @brief util function - 
/// @param start smaller than end
/// @param end bigger than start
/// @return 
float randfrang(float start, float end) {
    float gap = end - start;
    float rando = (float)rand() / (float)RAND_MAX;
    float answer = start + (gap * rando);
    return answer;
}

// Helper to compute tile index from world position
static void GetTileCoord(Vector3 pos, int cx, int cy, int *out_tx, int *out_ty) {
    float chunkOriginX = (cx - (CHUNK_COUNT/2)) * CHUNK_WORLD_SIZE;
    float chunkOriginZ = (cy - (CHUNK_COUNT/2)) * CHUNK_WORLD_SIZE;

    float localX = pos.x - chunkOriginX;
    float localZ = pos.z - chunkOriginZ;

    *out_tx = (int)(localX / TILE_WORLD_SIZE);
    *out_ty = (int)(localZ / TILE_WORLD_SIZE);

    *out_tx = Clamp(*out_tx, 0, TILE_GRID_SIZE - 1);
    *out_ty = Clamp(*out_ty, 0, TILE_GRID_SIZE - 1);
}

// Bake and export merged mesh from EnvObject array
void BakeTileObjects(int cx, int cy, int tx, int ty, EnvObject *objects, int count, const char * tileObjectType, Model_Type type) {
    EnsureDirectoryExists("map/");
    char chunkPath[64];
    snprintf(chunkPath, sizeof(chunkPath), "map/chunk_%02d_%02d/", cx, cy);
    
    EnsureDirectoryExists(chunkPath);

    char tile64Path[128];
    snprintf(tile64Path, sizeof(tile64Path), "%stile_64/", chunkPath);
    EnsureDirectoryExists(tile64Path);

    char folderPath[256];
    MakeTileFolderPath(folderPath, cx, cy, tx, ty);
    EnsureDirectoryExists(folderPath);

    // Merge all meshes into a single one
    int totalVertices = 0;
    int totalIndices = 0;
    for (int i = 0; i < count; i++) {
        if (objects[i].model.meshCount <= 0) { continue; }
        Mesh mesh = objects[i].model.meshes[0];
        totalVertices += mesh.vertexCount;
        totalIndices += mesh.triangleCount * 3;
    }

    float *vertices = (float *)malloc(totalVertices * 3 * sizeof(float));
    float *normals = (float *)malloc(totalVertices * 3 * sizeof(float));
    float *texcoords = (float *)malloc(totalVertices * 2 * sizeof(float));
    unsigned short *indices = (unsigned short *)malloc(totalIndices * sizeof(unsigned short));

    int vOffset = 0;
    int iOffset = 0;
    for (int i = 0; i < count; i++) {
        if (objects[i].model.meshCount <= 0) { continue; }
        Mesh mesh = objects[i].model.meshes[0];
        Matrix transform = objects[i].transform;

        if (mesh.vertices == NULL) {
            printf("ERROR: mesh data NULL in object %d\n", i);
        }
        if (mesh.indices == NULL) {
            printf("WANRING: indice data NULL in object %d\n", i);
        }
        if (mesh.triangleCount * 3 > totalIndices) {
            printf("WARNING: triangle count for object %d exceeds allocated index buffer\n", i);
        }


        for (int v = 0; v < mesh.vertexCount; v++) { //this might be a good place t implement down sampling skip
            Vector3 pos = {
                mesh.vertices[v*3 + 0],
                mesh.vertices[v*3 + 1],
                mesh.vertices[v*3 + 2]
            };
            pos = Vector3Transform(pos, transform);

            vertices[(vOffset + v)*3 + 0] = pos.x;
            vertices[(vOffset + v)*3 + 1] = pos.y;
            vertices[(vOffset + v)*3 + 2] = pos.z;

            normals[(vOffset + v)*3 + 0] = mesh.normals[v*3 + 0];
            normals[(vOffset + v)*3 + 1] = mesh.normals[v*3 + 1];
            normals[(vOffset + v)*3 + 2] = mesh.normals[v*3 + 2];

            texcoords[(vOffset + v)*2 + 0] = mesh.texcoords[v*2 + 0];
            texcoords[(vOffset + v)*2 + 1] = mesh.texcoords[v*2 + 1];
        }

        //if we do not have indices, create them
        if (mesh.indices == NULL)
        {
            int triangleCount = mesh.vertexCount / 3;  // Assume every 3 vertices form a triangle
            mesh.indices = (unsigned short *)MemAlloc(triangleCount * 3 * sizeof(unsigned short));
            
            for (int i = 0; i < triangleCount * 3; i++)
            {
                mesh.indices[i] = i;  // Simple 0, 1, 2, ..., n index buffer
            }

            mesh.triangleCount = triangleCount;
        }


        for (int t = 0; t < mesh.triangleCount * 3; t++) {
            //printf("indice data %d %d\n", iOffset + t, t);
            indices[iOffset + t] = vOffset + mesh.indices[t];
        }

        vOffset += mesh.vertexCount;
        iOffset += mesh.triangleCount * 3;
    }

    Mesh merged = { 0 };
    merged.vertexCount = totalVertices;
    merged.triangleCount = totalIndices / 3;
    merged.vertices = (float *)vertices;
    merged.normals = (float *)normals;
    merged.texcoords = (float *)texcoords;
    merged.indices = (unsigned short *)indices;

    UploadMesh(&merged, false);

    Model model = LoadModelFromMesh(merged);
    model.materials[0] = LoadMaterialDefault(); //sometimes I cant tell if what AI gives me is slop, or genius

    char modelPath[512];
    snprintf(modelPath, sizeof(modelPath), "%stile_%s_64.obj", folderPath, tileObjectType);
    EnsureDirectoryExists(folderPath);
    ExportMesh(merged, modelPath);
    //UnloadModel(model);
    UnloadMesh(merged);
    FILE *f = fopen("map/manifest.txt", "a"); // Open for append
    if (f != NULL) {
        fprintf(f, "%d %d %d %d %d %s\n", cx,cy,tx,ty,type,modelPath);  // modelPath is the filename you saved to
        fclose(f);
    }
    printf("Baked %d objects into %s\n", count, modelPath);
}

void ExportBatchTiles(int cx, int cy, StaticGameObject *props, int totalPropCount, int typePropCount, Model_Type mt) {
    // Step 1: Count trees per tile
    int tileCounts[TILE_GRID_SIZE][TILE_GRID_SIZE] = { 0 };
    for (int i = 0; i < totalPropCount; i++) {
        if(props[i].type==mt)
        {
            int tx, ty;
            GetTileCoord(props[i].pos, cx, cy, &tx, &ty);
            tileCounts[tx][ty]++;
        }
    }

    // Step 2: Process each tile
    for (int tx = 0; tx < TILE_GRID_SIZE; tx++) {
        for (int ty = 0; ty < TILE_GRID_SIZE; ty++) {
            int count = tileCounts[tx][ty];
            if (count == 0) continue;

            EnvObject *objects = (EnvObject *)malloc(sizeof(EnvObject) * count);
            if (!objects) {
                TraceLog(LOG_ERROR, "Out of memory batching tile %d,%d", tx, ty);
                continue;
            }

            int inserted = 0;
            for (int i = 0; i < totalPropCount; i++) {
                if(props[i].type==mt)
                {
                    int checkTx, checkTy;
                    GetTileCoord(props[i].pos, cx, cy, &checkTx, &checkTy);
                    if (checkTx == tx && checkTy == ty) {
                        EnvObject obj = { 0 };
                        obj.model = StaticObjectModels[mt];
                        obj.position = props[i].pos;
                        //obj.transform = MatrixTranslate(obj.position.x, obj.position.y, obj.position.z);
                        Matrix scaleMatrix = MatrixScale(props[i].scale, props[i].scale, props[i].scale);
                        Matrix pitchMatrix = MatrixRotateX(props[i].pitch);
                        Matrix yawMatrix   = MatrixRotateY(props[i].yaw);
                        Matrix rollMatrix  = MatrixRotateZ(props[i].roll);
                        Matrix rotationMatrix = MatrixMultiply(MatrixMultiply(pitchMatrix, yawMatrix), rollMatrix);
                        Matrix transform = MatrixMultiply(scaleMatrix, rotationMatrix);
                        transform = MatrixMultiply(transform, MatrixTranslate(obj.position.x, obj.position.y, obj.position.z));
                        obj.transform = transform;
                        objects[inserted++] = obj;
                    }
                }
            }

            BakeTileObjects(cx, cy, tx, ty, objects, inserted, GetModelName(mt), mt);
            free(objects);
        }
    }
}
//ding cookes are done hehe

Vector2 featurePoints[NUM_FEATURE_POINTS];
Image roadImage;
Image hardRoadMap;

Model chunkModels[CHUNK_COUNT][CHUNK_COUNT]; // 16x16 = 256 chunks
Model chunkModels32[CHUNK_COUNT][CHUNK_COUNT]; // 16x16 = 256 chunks
Model chunkModels16[CHUNK_COUNT][CHUNK_COUNT]; // 16x16 = 256 chunks
Model chunkModels8[CHUNK_COUNT][CHUNK_COUNT]; // 16x16 = 256 chunks

static inline float Clampf(float value, float min, float max) {
    return (value < min) ? min : (value > max) ? max : value;
}

static inline float smoothstep(float edge0, float edge1, float x)
{
    // Scale, bias, and saturate x to 0..1 range
    float t = Clampf((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    // Apply Hermite interpolation
    return t * t * (3.0f - 2.0f * t);
}

Color AverageColor(Color a, Color b) {
    Color result;
    result.r = (a.r + b.r) / 2;
    result.g = (a.g + b.g) / 2;
    result.b = (a.b + b.b) / 2;
    result.a = (a.a + b.a) / 2;
    return result;
}

float GetSlopeAt(int x, int y, Color *terrainData)
{
    // Avoid edges
    if (x <= 0 || y <= 0 || x >= ROAD_MAP_SIZE - 1 || y >= ROAD_MAP_SIZE - 1) return 0.0f;

    float hL = terrainData[y * ROAD_MAP_SIZE + (x - 1)].r / 255.0f;
    float hR = terrainData[y * ROAD_MAP_SIZE + (x + 1)].r / 255.0f;
    float hU = terrainData[(y - 1) * ROAD_MAP_SIZE + x].r / 255.0f;
    float hD = terrainData[(y + 1) * ROAD_MAP_SIZE + x].r / 255.0f;

    float dx = (hR - hL) * 0.5f;
    float dy = (hD - hU) * 0.5f;

    return sqrtf(dx * dx + dy * dy);
}

void GenerateWorleyRoadMap(Image *outImage, Image *hardMap, Color *terrainColorData, Color *terrainData) {
    const int SIZE = ROAD_MAP_SIZE;
    const float threshold = 0.78f;  // adjust for road width
    const float minEdge = 0.05f;  // Skip exact feature point centers

    for (int y = 0; y < SIZE; y++) {
        for (int x = 0; x < SIZE; x++) {
            float d1 = 1e9f, d2 = 1e9f;
            for (int i = 0; i < NUM_FEATURE_POINTS; i++) {
                float dx = (float)(featurePoints[i].x - x);
                float dy = (float)(featurePoints[i].y - y);
                float dist = dx*dx + dy*dy;

                if (dist < d1) {
                    d2 = d1;
                    d1 = dist;
                } else if (dist < d2) {
                    d2 = dist;
                }
            }
            Color baseColor = terrainColorData[y * ROAD_MAP_SIZE + x];

            // Classify undesirable surfaces
            bool isBadSurface =
            // Water (b >= 180 and g >= 60) — approximate, since water is dynamic
            (baseColor.b >= 180 && baseColor.g >= 60) ||
            // Sand
            (baseColor.r == 194 && baseColor.g == 178 && baseColor.b == 128) ||
            // Snow
            (baseColor.r == 255 && baseColor.g == 255 && baseColor.b == 255);

            float slope = GetSlopeAt(x, y, terrainData);
            bool isTooSteep = slope > 0.3f; // tweak this - note, 0-roughtly 0.707, 0.42 is a little high, roughly 60% slope//

            float edgeVal = fabsf(sqrtf(d2) - sqrtf(d1));
            if (edgeVal < threshold && edgeVal > minEdge && !isBadSurface && !isTooSteep) {
                ImageDrawPixel(outImage, x, y, (Color){ 140, 120, 100, 255 }); // dirt
                ImageDrawPixel(hardMap, x, y, WHITE); // road mask
            }
        }
    }
}

void ExpandRoadPaths(Image *hardMap, int radius) {
    Color *pixels = (Color *)hardMap->data;
    Color *copy = malloc(sizeof(Color) * ROAD_MAP_SIZE * ROAD_MAP_SIZE);
    memcpy(copy, pixels, sizeof(Color) * ROAD_MAP_SIZE * ROAD_MAP_SIZE);

    for (int y = 0; y < ROAD_MAP_SIZE; y++) {
        for (int x = 0; x < ROAD_MAP_SIZE; x++) {
            int idx = y * ROAD_MAP_SIZE + x;
            if (copy[idx].r > 200) {
                // Expand out in a square radius
                for (int dy = -radius; dy <= radius; dy++) {
                    for (int dx = -radius; dx <= radius; dx++) {
                        int nx = x + dx;
                        int ny = y + dy;
                        if ((unsigned)nx >= ROAD_MAP_SIZE || (unsigned)ny >= ROAD_MAP_SIZE) continue;
                        pixels[ny * ROAD_MAP_SIZE + nx] = WHITE;
                    }
                }
            }
        }
    }

    free(copy);
}

void FilterSmallRoadBlobs(Image *hardMap, int minSize) 
{
    Color *pixels = (Color *)hardMap->data;  // in-place
    bool *visited = calloc(ROAD_MAP_SIZE * ROAD_MAP_SIZE, sizeof(bool));
    int total = ROAD_MAP_SIZE * ROAD_MAP_SIZE;
    int *stack = malloc(sizeof(int) * total);
    int *blob  = malloc(sizeof(int) * total);

    for (int y = 0; y < ROAD_MAP_SIZE; y++) {
        for (int x = 0; x < ROAD_MAP_SIZE; x++) {
            int idx = y * ROAD_MAP_SIZE + x;
            if (visited[idx] || pixels[idx].r <= 200) continue;

            int stackSize = 0, blobSize = 0;
            stack[stackSize++] = idx;

            while (stackSize > 0) {
                int cur = stack[--stackSize];
                if (visited[cur]) continue;
                visited[cur] = true;
                blob[blobSize++] = cur;

                int cx = cur % ROAD_MAP_SIZE;
                int cy = cur / ROAD_MAP_SIZE;

                for (int oy = -1; oy <= 1; oy++) {
                    for (int ox = -1; ox <= 1; ox++) {
                        if (!ox && !oy) continue;
                        int nx = cx + ox;
                        int ny = cy + oy;
                        if ((unsigned)nx >= ROAD_MAP_SIZE || (unsigned)ny >= ROAD_MAP_SIZE) continue;
                        int ni = ny * ROAD_MAP_SIZE + nx;
                        if (!visited[ni] && pixels[ni].r > 200) {
                            stack[stackSize++] = ni;
                        }
                    }
                }
            }

            if (blobSize < minSize) {
                for (int i = 0; i < blobSize; i++) pixels[blob[i]] = BLACK;
            }
        }
    }

    free(stack);
    free(blob);
    free(visited);
}


void ClampMeshEdges(Model chunks[CHUNK_COUNT][CHUNK_COUNT], int lodSize) {
    for (int cy = 0; cy < CHUNK_COUNT; cy++) {
        for (int cx = 0; cx < CHUNK_COUNT; cx++) {
            Mesh *mesh = &(chunks[cx][cy].meshes[0]);
            float *verts = mesh->vertices;

            if (!verts || mesh->vertexCount != lodSize * lodSize) continue;

            // Left edge
            if (cx > 0) {
                float *leftVerts = chunks[cx - 1][cy].meshes[0].vertices;
                for (int y = 0; y < lodSize; y++) {
                    int i = 0;
                    int index = (y * lodSize + i) * 3 + 1;
                    int neighborIndex = (y * lodSize + (lodSize - 1)) * 3 + 1;
                    verts[index] = leftVerts[neighborIndex];
                }
            }

            // Right edge
            if (cx < CHUNK_COUNT - 1) {
                float *rightVerts = chunks[cx + 1][cy].meshes[0].vertices;
                for (int y = 0; y < lodSize; y++) {
                    int i = lodSize - 1;
                    int index = (y * lodSize + i) * 3 + 1;
                    int neighborIndex = (y * lodSize + 0) * 3 + 1;
                    verts[index] = rightVerts[neighborIndex];
                }
            }

            // Top edge
            if (cy > 0) {
                float *topVerts = chunks[cx][cy - 1].meshes[0].vertices;
                for (int x = 0; x < lodSize; x++) {
                    int j = 0;
                    int index = (j * lodSize + x) * 3 + 1;
                    int neighborIndex = ((lodSize - 1) * lodSize + x) * 3 + 1;
                    verts[index] = topVerts[neighborIndex];
                }
            }

            // Bottom edge
            if (cy < CHUNK_COUNT - 1) {
                float *bottomVerts = chunks[cx][cy + 1].meshes[0].vertices;
                for (int x = 0; x < lodSize; x++) {
                    int j = lodSize - 1;
                    int index = (j * lodSize + x) * 3 + 1;
                    int neighborIndex = (0 * lodSize + x) * 3 + 1;
                    verts[index] = bottomVerts[neighborIndex];
                }
            }

            // Re-upload to GPU
            UploadMesh(mesh, false);
        }
    }
}

float GetNoiseValue(float x, float y, float frequency, int octaves, int seed, float lacunarity)
{
    float total = 0.0f;
    float amplitude = 1.0f;
    float maxValue = 0.0f;

    for (int i = 0; i < octaves; i++) {
        total += stb_perlin_noise3(x * frequency, y * frequency, seed * 0.01f, 0, 0, 0) * amplitude;
        maxValue += amplitude;

        amplitude *= 0.5f;
        frequency *= lacunarity;
    }

    return total / maxValue;
}

void GenerateHeightmap(float *heightData, int width, int height, float scale, float frequency, int octaves, int seed, float lacunarity)
{
    // Params — let these be tweakable via keys
    float baseFreq = frequency;
    int baseOctaves = octaves;
    float baseLacunarity = lacunarity;

    float detailFreq = frequency * 4.0f;
    int detailOctaves = 2;
    float detailLacunarity = lacunarity * 1.5f;

    float detailStrength = 0.3f;
    float detailMaskStart = 0.3f;
    float detailMaskEnd = 0.7f;

    float heightAmplify = 1.5f;
    float elevationOffset = 0.2f;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            float nx = ((float)x - width / 2.0f) / width * scale;
            float ny = ((float)y - height / 2.0f) / height * scale;

            float base = GetNoiseValue(nx, ny, baseFreq, baseOctaves, seed, baseLacunarity);
            float detail = GetNoiseValue(nx, ny, detailFreq, detailOctaves, seed+100, detailLacunarity);

            float baseNorm = (base + 1.0f) * 0.5f;
            float mask = smoothstep(detailMaskStart, detailMaskEnd, baseNorm);

            float val = base + detail * detailStrength * mask;

            val = val * heightAmplify + elevationOffset;
            heightData[y * width + x] = Clampf(val, -1.0f, 1.0f);
        }
    }
}

// Assumes: GetNoiseValue(float nx, float ny, float freq, int octaves, int seed, float lacunarity)
// Assumes raylib-style Image/Color; adjust Image fields if your type differs.

//static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline float smoothstepf(float e0, float e1, float x) {
    float t = Clampf((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
static inline unsigned char u8(float x) { int i = (int)(x * 255.0f + 0.5f); if(i<0)i=0; if(i>255)i=255; return (unsigned char)i; }

// static inline Color ColorLerp(Color a, Color b, float t) {
//     return (Color){
//         (unsigned char)(a.r + (b.r - a.r) * t),
//         (unsigned char)(a.g + (b.g - a.g) * t),
//         (unsigned char)(a.b + (b.b - a.b) * t),
//         (unsigned char)(a.a + (b.a - a.a) * t)
//     };
// }

// Generate a color image from 2D Perlin (or simplex) noise, using a rich multi-stop gradient.
// - img: output Image (filled by this function)
// - width/height: image size
// - scale: spatial scale of the noise domain (larger -> zoomed out)
// - frequency/octaves/seed/lacunarity: passed to your GetNoiseValue() (returns [-1..1])
#define STOP_COUNT 11
void GenerateFoliageMap(Image *img, int width, int height,
                                 float scale, float frequency,
                                 int octaves, int seed, float lacunarity)
{
    // --- 1) Define a broad color gradient (wraps through the spectrum) ---
    // Order is circular so we get a near "full" color spread:
    // red -> orange -> yellow -> lime -> green -> cyan -> blue -> indigo -> violet -> magenta -> red
    // Color stops[STOP_COUNT] = {
    //     (Color){255,   0,   0,255}, // 0: red --mayber use this if we add a lot more biomes
    //     (Color){255, 128,   0,255}, // 1: orange
    //     (Color){255, 255,   0,255}, // 2: yellow
    //     (Color){128, 255,   0,255}, // 3: yellow-lime
    //     (Color){  0, 255,   0,255}, // 4: green
    //     (Color){  0, 255, 255,255}, // 5: cyan
    //     (Color){  0,   0, 255,255}, // 6: blue
    //     (Color){ 75,   0, 130,255}, // 7: indigo
    //     (Color){148,   0, 211,255}, // 8: violet
    //     (Color){255,   0, 255,255}, // 9: magenta
    //     (Color){255,   0,   0,255}  // 10: back to red (wrap)
    // };

    Color stops[STOP_COUNT] = {
        (Color){255,   0,   0,255}, // 0: red
        (Color){120,200,120 , 255}, //grass simple - Light desaturated green (pastel grass)  
        (Color){130,210,13, 255}, //grass full - Light yellow-green (fresh grass)  
        (Color){80,100, 200, 255 }, //dead - Dark olive green - blue shift
        (Color){10, 90, 40, 255},    //pine - Very dark green with a teal tint
        (Color){ 200,139, 34, 255 }, //BIOME_FOREST - forest green - shifted red   
        (Color){150,150,150, 255 }, //mtn 1 - Light gray
        (Color){ 40,250, 40, 255 }, ///ecc - Medium-dark green (slightly muted) - shifted green
        (Color){120,120,120 }, //mtn 2 - medium gray
        (Color){255,   0, 255,255}, // 9: magenta
        (Color){255,   0,   0,255}  // 10: back to red (wrap)
    };

    // --- 2) Allocate pixel buffer ---
    Color *pixels = (Color *)MemAlloc(sizeof(Color) * width * height);
    if (!pixels) {
        TraceLog(LOG_ERROR, "GenerateColorNoiseImageFull: Out of memory for %dx%d", width, height);
        img->data = NULL; img->width = img->height = img->mipmaps = img->format = 0;
        return;
    }

    // Optional: gentle contrast curve so mid-values don’t dominate too much
    const float contrast = 1.10f;     // >1 increases contrast a bit
    const float gammaInv = 1.0f/1.10f; // slight gamma correction

    // --- 3) Fill pixels ---
    for (int y = 0; y < height; ++y) {
        // normalized coords centered, scaled by `scale`
        float ny = ((float)y - height * 0.5f) / (float)height * scale;

        for (int x = 0; x < width; ++x) {
            float nx = ((float)x - width  * 0.5f) / (float)width  * scale;

            // Your noise function: expected output in [-1..1]
            float n = GetNoiseValue(nx, ny, frequency, octaves, seed, lacunarity);

            // Normalize to [0..1]
            float t = 0.5f * (n + 1.0f);

            // Small tone curve: contrast + gamma (all inline, no helpers)
            // Contrast about 0.5
            t = (t - 0.5f) * contrast + 0.5f;
            if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
            // Gamma
            t = powf(t, gammaInv);

            // Map t into gradient segment
            float pos   = t * (STOP_COUNT - 1);
            int   i0    = (int)pos;
            if (i0 >= STOP_COUNT - 1) i0 = STOP_COUNT - 2;
            int   i1    = i0 + 1;
            float u     = pos - (float)i0;  // local [0..1] within the segment

            // Lerp between stops[i0] and stops[i1]
            Color a = stops[i0];
            Color b = stops[i1];
            // channel-wise interpolation
            unsigned char r = (unsigned char)((1.0f - u) * a.r + u * b.r);
            unsigned char g = (unsigned char)((1.0f - u) * a.g + u * b.g);
            unsigned char bch= (unsigned char)((1.0f - u) * a.b + u * b.b);

            pixels[y*width + x] = (Color){ r, g, bch, 255 };
        }
    }

    // --- 4) Fill out the Image struct (raylib style) ---
    img->data    = pixels;
    img->width   = width;
    img->height  = height;
    img->mipmaps = 1;
    img->format  = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8; // RGBA8
}

void ApplyFastBoxBlur(Color *pixels, int width, int height, int kernelSize, bool useAvg) {
    int step = kernelSize / 4;
    if (step < 1) step = 1;

    Color *copy = malloc(sizeof(Color) * width * height);
    memcpy(copy, pixels, sizeof(Color) * width * height);

    for (int y = 0; y < height; y += step) {
        //TraceLog(LOG_INFO, "y even bother ? %d", y);
        for (int x = 0; x < width; x += step) {
            int r = 0, g = 0, b = 0, count = 0;

            for (int dy = -kernelSize / 2; dy <= kernelSize / 2; dy++) {
                for (int dx = -kernelSize / 2; dx <= kernelSize / 2; dx++) {
                    int nx = x + dx;
                    int ny = y + dy;
                    if (nx >= 0 && ny >= 0 && nx < width && ny < height) {
                        Color c = copy[ny * width + nx];
                        r += c.r; g += c.g; b += c.b;
                        count++;
                    }
                }
            }

            Color avg = {
                .r = r / count,
                .g = g / count,
                .b = b / count,
                .a = 255
            };

            // Fill step x step block with the average color
            for (int fy = 0; fy < step; fy++) {
                for (int fx = 0; fx < step; fx++) {
                    int tx = x + fx;
                    int ty = y + fy;
                    if (tx < width && ty < height) {
                        if(useAvg)
                        {pixels[ty * width + tx] = AverageColor(avg, pixels[ty * width + tx]);}
                        else
                        {pixels[ty * width + tx] = avg;}
                    }
                }
            }
        }
    }

    free(copy);
}
//song 2
Image UpscaleImageBilinear(Image src, int newWidth, int newHeight) {
    Image img = GenImageColor(newWidth, newHeight, BLACK);
    Color *srcPixels = LoadImageColors(src);
    Color *newPixels = LoadImageColors(img);

    for (int y = 0; y < newHeight; y++) {
        for (int x = 0; x < newWidth; x++) {
            float gx = ((float)x / (newWidth - 1)) * (src.width - 1);
            float gy = ((float)y / (newHeight - 1)) * (src.height - 1);

            int x0 = (int)gx;
            int y0 = (int)gy;
            int x1 = (x0 + 1 < src.width)  ? x0 + 1 : x0;
            int y1 = (y0 + 1 < src.height) ? y0 + 1 : y0;

            float dx = gx - x0;
            float dy = gy - y0;

            Color c00 = srcPixels[y0 * src.width + x0];
            Color c10 = srcPixels[y0 * src.width + x1];
            Color c01 = srcPixels[y1 * src.width + x0];
            Color c11 = srcPixels[y1 * src.width + x1];

            Color result = {
                .r = (unsigned char)((1 - dx)*(1 - dy)*c00.r + dx*(1 - dy)*c10.r + (1 - dx)*dy*c01.r + dx*dy*c11.r),
                .g = (unsigned char)((1 - dx)*(1 - dy)*c00.g + dx*(1 - dy)*c10.g + (1 - dx)*dy*c01.g + dx*dy*c11.g),
                .b = (unsigned char)((1 - dx)*(1 - dy)*c00.b + dx*(1 - dy)*c10.b + (1 - dx)*dy*c01.b + dx*dy*c11.b),
                .a = 255
            };

            newPixels[y * newWidth + x] = result;
        }
    }

    //todo: probably put these back
    ApplyFastBoxBlur(newPixels, newWidth, newHeight, 32, false);
    ApplyFastBoxBlur(newPixels, newWidth, newHeight, 23, true);
    ApplyFastBoxBlur(newPixels, newWidth, newHeight, 7, false);

    UnloadImageColors(srcPixels);
    Image out = {
        .data = newPixels,
        .width = newWidth,
        .height = newHeight,
        .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8,
        .mipmaps = 1
    };

    return out;
}

void RebuildImageFromHeightData(Image *image, float *heightData, int width, int height)
{
    Color *pixels = (Color *)image->data;

    float min = FLT_MAX, max = -FLT_MAX;
    for (int i = 0; i < width * height; i++) {
        if (heightData[i] < min) min = heightData[i];
        if (heightData[i] > max) max = heightData[i];
    }
    float range = max - min;
    printf("Height range: %.3f to %.3f\n", min, max);
    if (range <= 0.0001f) range = 1.0f; // prevent divide by zero

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            float val = heightData[y * width + x];
            float norm = (val - min) / range; // map to [0,1]
            int gray = (int)(norm * 255.0f);
            pixels[y * width + x] = (Color){ gray, gray, gray, 255 };
        }
    }
}

void RebuildColorImageFromHeightData(Image *image, float *heightData, int width, int height)
{
    Color *pixels = (Color *)image->data;

    float min = FLT_MAX, max = -FLT_MAX;
    for (int i = 0; i < width * height; i++) {
        if (heightData[i] < min) min = heightData[i];
        if (heightData[i] > max) max = heightData[i];
    }
    float range = max - min;
    if (range <= 0.0001f) range = 1.0f;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            float val = heightData[y * width + x];
            float norm = (val - min) / range; // 0.0 to 1.0

            Color c;
            if (val <= 0.0f)
            {
                // Deeper = darker blue
                float depth = Clampf(-val, 0.0f, 1.0f); // Normalize depth range 0 to 1
                Color color = (Color){
                    (unsigned char)(20 + 30 * (1.0f - depth)), // R (lighter near surface)
                    (unsigned char)(60 + 80 * (1.0f - depth)), // G
                    (unsigned char)(180 + 75 * (1.0f - depth)), // B
                    255
                };
                c = color;
            } // water
            else if (val <= 0.05f)          c = (Color){ 194, 178, 128, 255 }; // sand
            else if (val <= 0.15f)          c = (Color){ 140, 190, 90, 255 };  // grass1
            else if (val <= 0.30f)          c = (Color){ 100, 160, 60, 255 };  // grass2
            else if (val <= 0.45f)          c = (Color){ 70, 110, 40, 255 };   // grass3
            else if (val <= 0.60f)          c = (Color){ 100, 100, 100, 255 }; // rock1
            else if (val <= 0.75f)          c = (Color){ 180, 180, 180, 255 }; // rock2
            else                            c = (Color){ 255, 255, 255, 255 }; // snow
            pixels[y * width + x] = c;
        }
    }
}

void RebuildSlopeImageFromHeightData(Image *image, float *heightData, int width, int height) 
{
    Color *pixels = (Color *)image->data;
    // Compute slope at each pixel
     for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = y * width + x;
            float h = heightData[idx];

            int xl = (x > 0) ? x - 1 : x;
            int xr = (x < width - 1) ? x + 1 : x;
            int yu = (y < height - 1) ? y + 1 : y;
            int yd = (y > 0) ? y - 1 : y;

            //float h = heightData[y * width + x];//todo, this feels wrong, remove?
            float hL = heightData[y * width + xl];
            float hR = heightData[y * width + xr];
            float hU = heightData[yu * width + x];
            float hD = heightData[yd * width + x];

            float dhdx = (hR - hL) * HEIGHT_SCALE / 2.0f;
            float dhdy = (hU - hD) * HEIGHT_SCALE / 2.0f;
            float slope = sqrtf(dhdx * dhdx + dhdy * dhdy);

            Color c;

            if (h < 0.0f) {
                // underwater rock/soil, darker with depth
                float t = Clamp(h / 0.2f, 0.0f, 1.0f);  // 0.0 = deep, 1.0 = shallow

                // Blend from deep = (50, 40, 30) to shallow = (150, 120, 80)
                int r = (int)(50 * (1 - t) + 160 * t);
                int g = (int)(40 * (1 - t) + 130 * t);
                int b = (int)(30 * (1 - t) + 106 * t);

                c = (Color){ r, g, b, 255 };
            }
            else if (h > 0.87f && slope < 0.3f) {
                // snowy highlands
                int whiteness = 230 + (int)(slope * 20.0f);
                if (whiteness > 255) whiteness = 255;
                c = (Color){whiteness, whiteness, whiteness, 255};
            }
            else if (slope < 0.45f) {
                // grassier plains
                int green = 90 + (int)(h * 80.0f);  // range 90–170ish
                if (green > 200) green = 200;
                c = (Color){30, green, 30, 255};
            }
            else {
                // rocky
                int gray = 70 + (int)(slope * 50.0f);
                if (gray > 180) gray = 180;
                c = (Color){gray, gray, gray - 10, 255};
            }

            pixels[idx] = c;
        }
    }
}

Image AverageImages(Image imgA, Image imgB)
{
    // Ensure same dimensions
    if (imgA.width != imgB.width || imgA.height != imgB.height) {
        TraceLog(LOG_ERROR, "Images must be the same size to average");
        return GenImageColor(1, 1, BLACK);  // dummy fallback
    }

    Image result = GenImageColor(imgA.width, imgA.height, BLACK);
    Color *pixelsA = LoadImageColors(imgA);
    Color *pixelsB = LoadImageColors(imgB);
    Color *resultPixels = LoadImageColors(result);

    for (int i = 0; i < imgA.width * imgA.height; i++) {
        resultPixels[i].r = (pixelsA[i].r + pixelsB[i].r) / 2;
        resultPixels[i].g = (pixelsA[i].g + pixelsB[i].g) / 2;
        resultPixels[i].b = (pixelsA[i].b + pixelsB[i].b) / 2;
        resultPixels[i].a = (pixelsA[i].a + pixelsB[i].a) / 2;
    }

    Image newImage = {
        .data = resultPixels,
        .width = imgA.width,
        .height = imgA.height,
        .mipmaps = 1,
        .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8
    };

    // Clean up temporary pixel arrays (but not resultPixels, now owned by newImage)
    UnloadImageColors(pixelsA);
    UnloadImageColors(pixelsB);
    UnloadImage(result);  // we discard the black dummy pixels

    return newImage;
}

void ApplyHeightCurve(float *heightData, int width, int height, float strength, float center, float radius)
{
    for (int i = 0; i < width * height; i++) {
        float h = heightData[i];                 // [-1, 1]
        float norm = (h + 1.0f) * 0.5f;           // [0, 1]

        // Mask: peak at center, falloff over radius
        float d = (norm - center) / radius;
        float mask = 1.0f - d * d;
        mask = fmaxf(0.0f, mask);                 // clamp

        // Flatten toward center
        norm = norm + (center - norm) * mask * strength;

        // Remap back to [-1, 1]
        heightData[i] = norm * 2.0f - 1.0f;
    }
}

void ApplyHeightSigmoidFilter(float *heightData, int width, int height, float strength, float center, float radius)
{
    for (int i = 0; i < width * height; i++) {
        float h = heightData[i];                 // [-1, 1]
        float norm = (h + 1.0f) * 0.5f;           // [0, 1]

        // Mask: peak at center, falloff over radius
        float d = (norm - center) / radius;
        float mask = 1.0f - d * d;
        mask = fmaxf(0.0f, mask);                 // clamp

        // Sigmoid curve to flatten mid-values, push outer slopes
        float shaped = 1.0f / (1.0f + expf(-12.0f * (norm - 0.5f))); // sharper S-curve
        norm = norm + (shaped - norm) * mask * strength;

        heightData[i] = norm * 2.0f - 1.0f;       // back to [-1, 1]
    }
}

void ApplyHeightAntiSigmoidFilter(float *heightData, int width, int height, float strength, float center, float radius)
{
    for (int i = 0; i < width * height; i++) {
        float h = heightData[i];                 // [-1, 1]
        float norm = (h + 1.0f) * 0.5f;           // [0, 1]

        // Mask: peak at center, falloff over radius
        float d = (norm - center) / radius;
        float mask = 1.0f - d * d;
        mask = fmaxf(0.0f, mask);                 // clamp

        // Steep-flat-steep: Invert a sigmoid by bending *away* from the middle
        float sigmoid = 1.0f / (1.0f + expf(-12.0f * (norm - 0.5f)));
        float shaped = norm + ((norm - sigmoid) * 2.0f);  // exaggerate the difference

        // Blend toward shaped version
        norm = norm + (shaped - norm) * mask * strength;

        heightData[i] = norm * 2.0f - 1.0f;       // back to [-1, 1]
    }
}

void ApplyErosion(float *heightData, int width, int height, int steps, float erosionFactor)
{
    for (int s = 0; s < steps; s++) {
        for (int y = 1; y < height - 1; y++) {
            for (int x = 1; x < width - 1; x++) {
                int idx = y * width + x;
                float current = heightData[idx];
                float totalDiff = 0.0f;
                float diffs[4];
                int offsets[4] = { -width, +width, -1, +1 };

                for (int i = 0; i < 4; i++) {
                    float neighbor = heightData[idx + offsets[i]];
                    float diff = current - neighbor;
                    diffs[i] = (diff > 0.0f) ? diff : 0.0f;
                    totalDiff += diffs[i];
                }

                if (totalDiff > 0.0f) {
                    float erosionAmount = fminf(current * erosionFactor, 0.01f); // cap to 0.01f
                    for (int i = 0; i < 4; i++) {
                        float transfer = erosionAmount * (diffs[i] / totalDiff);
                        heightData[idx] -= transfer;
                        heightData[idx + offsets[i]] += transfer;
                    }
                }
            }
        }
    }
}

void ApplyErosionHydraulic(float *heightData, int width, int height)
{
    for (int i = 0; i < EROSION_DROPLETS; i++) {
        float posX = (float)(rand() % (width - 2)) + 1;
        float posY = (float)(rand() % (height - 2)) + 1;

        float dirX = 0.0f;
        float dirY = 0.0f;
        float speed = 1.0f;
        float water = 1.0f;
        float sediment = 0.0f;

        for (int lifetime = 0; lifetime < EROSION_LIFETIME; lifetime++) {
            int x0 = (int)posX;
            int y0 = (int)posY;
            float fx = posX - x0;
            float fy = posY - y0;

            // Bilinear height sampling
            int idx = y0 * width + x0;
            float h00 = heightData[idx];
            float h10 = heightData[idx + 1];
            float h01 = heightData[idx + width];
            float h11 = heightData[idx + width + 1];

            float height = (1 - fx) * (1 - fy) * h00 +
                           fx * (1 - fy) * h10 +
                           (1 - fx) * fy * h01 +
                           fx * fy * h11;

            // Gradient
            float gradX = (h10 - h00) * (1 - fy) + (h11 - h01) * fy;
            float gradY = (h01 - h00) * (1 - fx) + (h11 - h10) * fx;

            dirX = dirX * INERTIA - gradX * (1 - INERTIA);
            dirY = dirY * INERTIA - gradY * (1 - INERTIA);

            float len = sqrtf(dirX * dirX + dirY * dirY);
            if (len != 0.0f) {
                dirX /= len;
                dirY /= len;
            }

            posX += dirX;
            posY += dirY;

            if (posX < 1 || posX >= width - 2 || posY < 1 || posY >= height - 2)
                break;

            int newIdx = ((int)posY) * width + (int)posX;
            float newHeight = heightData[newIdx];
            float deltaHeight = height - newHeight;

            float capacity = fmaxf(-deltaHeight * speed * water * CAPACITY_MULTIPLIER, 0.01f);

            if (sediment > capacity) {
                float deposit = (sediment - capacity) * DEPOSITION_SPEED;
                heightData[newIdx] += deposit;
                sediment -= deposit;
            } else {
                float erosionAmount = fminf((capacity - sediment) * EROSION_SPEED, newHeight - MIN_HEIGHT);
                heightData[newIdx] -= erosionAmount;
                sediment += erosionAmount;
            }

            speed = sqrtf(speed * speed + deltaHeight * GRAVITY);
            water *= (1 - EVAPORATE_SPEED);
            if (water <= 0.01f) break;
        }
    }
}

void ApplyBorderFade(float *heightData, int width, int height, float edgeFadeStrength, float centerLift, float trenchDepth)
{
    for (int y = 0; y < height; y++)
    {
        for (int x = 0; x < width; x++)
        {
            int i = y * width + x;

            // Distance from center (0 = center, 1 = edge)
            float dx = (float)(x - width / 2) / (width / 2);
            float dy = (float)(y - height / 2) / (height / 2);
            float dist = sqrtf(dx * dx + dy * dy);
            float normDist = fminf(dist, 1.0f);

            // Fade height toward deep trench at edge
            float fade = powf(normDist, 2.0f); // soften falloff
            float h = heightData[i];

            // Sinks toward trenchDepth instead of 0
            h = h * (1.0f - fade * edgeFadeStrength) + trenchDepth * fade * edgeFadeStrength;

            // Optionally lift center
            float lift = powf(1.0f - normDist, 2.0f);
            h += lift * centerLift;

            heightData[i] = h;
        }
    }
}

float SimpleNoise2D(float x, float y)
{
    return sinf(x * 12.9898f + y * 78.233f) * 43758.5453f - floorf(sinf(x * 12.9898f + y * 78.233f) * 43758.5453f);
}

float GetFractalNoise(float x, float y, int octaves, float frequency, float lacunarity, float gain, int seed)
{
    float sum = 0.0f;
    float amp = 1.0f;
    float freq = frequency;

    for (int i = 0; i < octaves; i++) {
        sum += GetNoiseValue(x * freq, y * freq, 1, 1, seed + i, 1.0f) * amp;
        freq *= lacunarity;  // usually 2.0
        amp *= gain;         // usually 0.5
    }

    return sum;
}

void ApplySmoothNoiseOverlay(float *heightData, int width, int height, float amplitude, float frequency)
{
    for (int y = 0; y < height; y++)
    {
        for (int x = 0; x < width; x++)
        {
            int i = y * width + x;

            float nx = (float)x / (float)width;
            float ny = (float)y / (float)height;
            float noise = GetFractalNoise(nx, ny, 5, 2.0f, 2.0f, 0.5f, 1337);

            // Remap to -1..1
            noise = (noise - 0.5f) * 2.0f;

            // Blend onto terrain
            heightData[i] += noise * 0.02f;
            heightData[i] = Clamp(heightData[i], 0.0f, 1.0f);
        }
    }
}

void ApplyHeightNegativeFilter(float *heightData, int width, int height, float strength)
{
    for (int i = 0; i < width * height; i++) {
        float h = heightData[i];        // Original height [-1, 1]
        float neg = -h;                 // Flip it vertically
        heightData[i] = h + (neg - h) * strength;
        // Same as: heightData[i] = Lerp(h, -h, strength);
    }
}


void ImageDataFlipVertical(Image *image) {
    int width = image->width;
    int height = image->height;
    int bytesPerPixel = 4;  // Assuming RGBA8
    int stride = width * bytesPerPixel;

    unsigned char *pixels = (unsigned char *)image->data;
    unsigned char *tempRow = (unsigned char *)malloc(stride);

    for (int y = 0; y < height / 2; y++) {
        unsigned char *row1 = pixels + y * stride;
        unsigned char *row2 = pixels + (height - 1 - y) * stride;

        memcpy(tempRow, row1, stride);
        memcpy(row1, row2, stride);
        memcpy(row2, tempRow, stride);
    }

    free(tempRow);
}

void ImageDataFlipHorizontal(Image *image) {
    int width = image->width;
    int height = image->height;
    int bytesPerPixel = 4;  // Assuming RGBA8
    unsigned char *pixels = (unsigned char *)image->data;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width / 2; x++) {
            int i1 = (y * width + x) * bytesPerPixel;
            int i2 = (y * width + (width - 1 - x)) * bytesPerPixel;

            for (int b = 0; b < bytesPerPixel; b++) {
                unsigned char tmp = pixels[i1 + b];
                pixels[i1 + b] = pixels[i2 + b];
                pixels[i2 + b] = tmp;
            }
        }
    }
}

// Barycentric interpolation to get Y at point (x, z) on triangle
float GetHeightOnTriangle(Vector3 p, Vector3 a, Vector3 b, Vector3 c)
{
    // Convert to 2D XZ plane
    float px = p.x, pz = p.z;

    float ax = a.x, az = a.z;
    float bx = b.x, bz = b.z;
    float cx = c.x, cz = c.z;

    // Compute vectors
    float v0x = bx - ax;
    float v0z = bz - az;
    float v1x = cx - ax;
    float v1z = cz - az;
    float v2x = px - ax;
    float v2z = pz - az;

    // Compute dot products
    float d00 = v0x * v0x + v0z * v0z;
    float d01 = v0x * v1x + v0z * v1z;
    float d11 = v1x * v1x + v1z * v1z;
    float d20 = v2x * v0x + v2z * v0z;
    float d21 = v2x * v1x + v2z * v1z;

    // Compute barycentric coordinates
    float denom = d00 * d11 - d01 * d01;
    if (denom == 0.0f)
    {
        //TraceLog(LOG_INFO, "denom == 0");
        return -10000.0f;
    }

    float v = (d11 * d20 - d01 * d21) / denom;
    float w = (d00 * d21 - d01 * d20) / denom;
    float u = 1.0f - v - w;

    // If point is outside triangle
    if (u < 0 || v < 0 || w < 0)
    {
        //TraceLog(LOG_INFO, "Outside of plane (%.2f,%.2f,%.2f)", u, v, w);
        return -10000.0f;
    }

    // Interpolate Y
    return u * a.y + v * b.y + w * c.y;
}


float GetTerrainHeightFromMeshXZ(Mesh mesh, Vector3 chnkPos, float x, float z)
{
    float *verts = (float *)mesh.vertices;
    unsigned short *tris = (unsigned short *)mesh.indices;
    if (!verts || mesh.vertexCount < 3 || mesh.triangleCount < 1)
    {
        TraceLog(LOG_WARNING, "Something wrong with collision: (%f x %f)", x, z);
        if(!verts){TraceLog(LOG_WARNING, "!verts");}
        if(mesh.vertexCount < 3){TraceLog(LOG_WARNING, "mesh.vertexCount < 3");}
        if(mesh.triangleCount < 1){TraceLog(LOG_WARNING, "mesh.triangleCount < 1");}
        return -10000.0f;
    }

    Vector3 p = { x, 0.0f, z };

    for (int i = 0; i < mesh.triangleCount; i++) {
        int i0, i1, i2;

        if (tris) {
            i0 = tris[i * 3 + 0];
            i1 = tris[i * 3 + 1];
            i2 = tris[i * 3 + 2];
        } else {
            i0 = i * 3 + 0;
            i1 = i * 3 + 1;
            i2 = i * 3 + 2;
        }

        if (i0 >= mesh.vertexCount || i1 >= mesh.vertexCount || i2 >= mesh.vertexCount){continue;}

        Vector3 a = {
            (MAP_SCALE * verts[i0 * 3 + 0] + chnkPos.x),
            (MAP_SCALE * verts[i0 * 3 + 1] + chnkPos.y),
            (MAP_SCALE * verts[i0 * 3 + 2] + chnkPos.z)
        };
        Vector3 b = {
            (MAP_SCALE * verts[i1 * 3 + 0] + chnkPos.x),
            (MAP_SCALE * verts[i1 * 3 + 1] + chnkPos.y),
            (MAP_SCALE * verts[i1 * 3 + 2] + chnkPos.z)
        };
        Vector3 c = {
            (MAP_SCALE * verts[i2 * 3 + 0] + chnkPos.x),
            (MAP_SCALE * verts[i2 * 3 + 1] + chnkPos.y),
            (MAP_SCALE * verts[i2 * 3 + 2] + chnkPos.z)
        };
        //TraceLog(LOG_INFO, "Tri %d verts: a=(%.2f,%.2f,%.2f)", i, a.x, a.y, a.z);
        //TraceLog(LOG_INFO, "Tri %d verts: b=(%.2f,%.2f,%.2f)", i, b.x, b.y, b.z);
        //TraceLog(LOG_INFO, "Tri %d verts: c=(%.2f,%.2f,%.2f)", i, c.x, c.y, c.z);
        float y = GetHeightOnTriangle((Vector3){x, 0, z}, a, b, c);
        if (y > -9999.0f) return y;
    }

    TraceLog(LOG_WARNING, "Not found in any triangle: (%f x %f)", x, z);
    return -10000.0f; // Not found in any triangle
}

Image SampleImageDown(Image src, int targetSize)
{
    Image result = GenImageColor(targetSize, targetSize, BLACK);
    Color *srcPixels = LoadImageColors(src);
    Color *dstPixels = LoadImageColors(result);

    int srcW = src.width;
    int srcH = src.height;

    for (int y = 0; y < targetSize; y++) {
        for (int x = 0; x < targetSize; x++) {
            int srcX = (x * (srcW - 1)) / (targetSize - 1);
            int srcY = (y * (srcH - 1)) / (targetSize - 1);
            dstPixels[y * targetSize + x] = srcPixels[srcY * srcW + srcX];
        }
    }

    UnloadImageColors(srcPixels);
    Image dst = {
        .data = dstPixels,
        .width = targetSize,
        .height = targetSize,
        .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8,
        .mipmaps = 1
    };
    return dst;
}

///
//This guy is barely hanging on for life
///
Model GenerateChunkModel(float *heightData, Image colorImage, Color *colorData, int mapSize, int chunkX, int chunkY, int chunkSize, float heightScale)
{
    int vertsPerChunk = chunkSize + 1;

    // Prepare subset color image for this chunk
    Color *chunkPixels = malloc(sizeof(Color) * vertsPerChunk * vertsPerChunk);
    Color *chunkHeights = malloc(sizeof(Color) * vertsPerChunk * vertsPerChunk);
    Color *fullPixels = LoadImageColors(colorImage);  // read from input image

    for (int y = 0; y <= chunkSize; y++) {
        for (int x = 0; x <= chunkSize; x++) {
            int globalX = chunkX * chunkSize + x;
            int globalY = chunkY * chunkSize + y;

            int srcIndex = globalY * mapSize + globalX;
            int dstIndex = y * vertsPerChunk + x;
            if (dstIndex >= (vertsPerChunk * vertsPerChunk)) { continue;}
            if (srcIndex >= (colorImage.height * colorImage.width)) { continue; }
            chunkPixels[dstIndex] = fullPixels[srcIndex];
        }
    }

    UnloadImageColors(fullPixels); // done with full source pixels

    for (int y = 0; y <= chunkSize; y++) {
        for (int x = 0; x <= chunkSize; x++) {
            int globalX = chunkX * chunkSize + x;
            int globalY = chunkY * chunkSize + y;

            int srcIndex = globalY * mapSize + globalX;
            int dstIndex = y * vertsPerChunk + x;
            if (dstIndex >= (vertsPerChunk * vertsPerChunk)) { continue; }
            if (srcIndex >= (MAP_SIZE * MAP_SIZE)) { continue; }
            chunkHeights[dstIndex] = colorData[srcIndex];
        }
    }

    Image chunkImage = {
        .data = chunkHeights,
        .width = vertsPerChunk,
        .height = vertsPerChunk,
        .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8,
        .mipmaps = 1
    };

    Image chunkColorImage = {
        .data = chunkPixels,
        .width = vertsPerChunk,
        .height = vertsPerChunk,
        .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8,
        .mipmaps = 1
    };

    // Generate mesh and texture
    //LOD stuff
    // Image img32 = ImageCopy(chunkImage); // make a copy for GenMeshHeightmap
    // Image img16 = ImageCopy(chunkImage); // make a copy for GenMeshHeightmap
    // Image img8 = ImageCopy(chunkImage); // make a copy for GenMeshHeightmap

    // Image img32 = SubsampleHeightmap(chunkImage, 2);  // for 33x33
    // Image img16 = SubsampleHeightmap(chunkImage, 4);  // for 33x33
    // Image img8 = SubsampleHeightmap(chunkImage, 8);  // for 33x33

    // ImageResize(&img32,33,33);
    // ImageResize(&img16,17,17);
    // ImageResize(&img8,9,9);
    Image img32 = SampleImageDown(chunkImage, 33);
    Image img16 = SampleImageDown(chunkImage, 17);
    Image img8  = SampleImageDown(chunkImage, 9);

    Mesh mesh32 = GenMeshHeightmap(img32, (Vector3){ (float)chunkSize, heightScale, (float)chunkSize});
    Mesh mesh16 = GenMeshHeightmap(img16, (Vector3){ (float)chunkSize, heightScale, (float)chunkSize});
    Mesh mesh8 = GenMeshHeightmap(img8, (Vector3){ (float)chunkSize, heightScale, (float)chunkSize});
    Mesh mesh = GenMeshHeightmap(chunkImage, (Vector3){ (float)chunkSize, heightScale, (float)chunkSize });

    Model model32 = LoadModelFromMesh(mesh32);
    Model model16 = LoadModelFromMesh(mesh16);
    Model model8 = LoadModelFromMesh(mesh8);
    chunkModels32[chunkX][chunkY] = model32;
    chunkModels16[chunkX][chunkY] = model16;
    chunkModels8[chunkX][chunkY] = model8;

    Texture2D texture = LoadTextureFromImage(chunkColorImage);
    UnloadImage(img32); // unload metadata
    UnloadImage(img16); // unload metadata
    UnloadImage(img8); // unload metadata
    UnloadImage(chunkImage); // unload metadata
    UnloadImage(chunkColorImage); // unload metadata

    Model model = LoadModelFromMesh(mesh);
    model.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture = texture;

    return model;
}

void SaveTreePositions(int cx, int cy, StaticGameObject *props, int propsCount)
{
    char outPath[256];
    snprintf(outPath, sizeof(outPath), "map/chunk_%02d_%02d/trees.txt", cx, cy);

    FILE *fp = fopen(outPath, "w");
    if (!fp) {
        TraceLog(LOG_WARNING, "Failed to write tree file: %s", outPath);
        return;
    }

    fprintf(fp, "%d\n", propsCount);
    for (int i = 0; i < propsCount; i++) {
        fprintf(fp, "%.3f %.3f %.3f %d %.3f %.3f %.3f %.3f\n", 
            props[i].pos.x, props[i].pos.y, props[i].pos.z, props[i].type,
            props[i].yaw, props[i].pitch, props[i].roll, props[i].scale
        );
    }

    fclose(fp);
    TraceLog(LOG_INFO, "Wrote %d static props to %s", propsCount, outPath);
}

unsigned int HashCoords(int x, int y) {
    unsigned int h = (unsigned int)(x * 73856093) ^ (y * 19349663);
    h = (h ^ (h >> 13)) * 0x5bd1e995;
    return h;
}

//--MODEL BATCHING SECTION----------------------------------------------------------------------
//----------------------------------------------------------------------------------------------
Mesh BuildBatchMeshForTile(EnvObject *objects, int count) {
    int totalVertices = 0;
    int totalIndices = 0;

    // First, count how big our final buffers need to be
    for (int i = 0; i < count; i++) {
        Mesh mesh = objects[i].model.meshes[0];
        totalVertices += mesh.vertexCount;
        totalIndices += mesh.triangleCount * 3;
    }

    // Allocate final buffers
    float *vertices = RL_CALLOC(totalVertices * 3, sizeof(float)); // 3 floats per vertex
    float *texcoords = RL_CALLOC(totalVertices * 2, sizeof(float)); // 2 floats per uv
    unsigned short *indices = RL_CALLOC(totalIndices, sizeof(unsigned short));

    int vOffset = 0;
    int iOffset = 0;

    for (int i = 0; i < count; i++) {
        Mesh mesh = objects[i].model.meshes[0];
        Vector3 pos = objects[i].position;

        // Copy vertex positions
        for (int v = 0; v < mesh.vertexCount; v++) {
            float *src = &mesh.vertices[v * 3];
            vertices[(vOffset + v) * 3 + 0] = src[0] + pos.x;
            vertices[(vOffset + v) * 3 + 1] = src[1] + pos.y;
            vertices[(vOffset + v) * 3 + 2] = src[2] + pos.z;
        }

        // Copy texcoords
        for (int v = 0; v < mesh.vertexCount; v++) {
            float *src = &mesh.texcoords[v * 2];
            texcoords[(vOffset + v) * 2 + 0] = src[0];
            texcoords[(vOffset + v) * 2 + 1] = src[1];
        }

        // Copy indices, with offset
        for (int j = 0; j < mesh.triangleCount * 3; j++) {
            indices[iOffset + j] = mesh.indices[j] + vOffset;
        }

        vOffset += mesh.vertexCount;
        iOffset += mesh.triangleCount * 3;
    }

    // Build the mesh
    Mesh result = { 0 };
    result.vertexCount = totalVertices;
    result.triangleCount = totalIndices / 3;

    result.vertices = (float *)vertices;
    result.texcoords = (float *)texcoords;
    result.indices = (unsigned short *)indices;

    UploadMesh(&result, false); // send to VRAM

    return result;
}
//----------------------------------------------------------------------------------------------
//--END--//--MODEL BATCHING SECTION-------------------------------------------------------------

/// @brief this is actually for all static prop model types, and will bake cookies
/// @param chunkX 
/// @param chunkY 
/// @param heightData 
/// @param colorData 
/// @param mapSize 
/// @param heightScale 
void SaveChunkVegetationImage(int chunkX, int chunkY, float *heightData, Color *colorData, Color *foliageData, int mapSize, float heightScale)
{
    const int outSize = 1024;
    const int chunkSize = CHUNK_SIZE + 1;
    const float step = (float)CHUNK_SIZE / (float)(outSize - 1);

    Image vegImage = GenImageColor(outSize, outSize, BLACK);
    Color *pixels = (Color *)vegImage.data;

    for (int y = 0; y < outSize; y++) {
        for (int x = 0; x < outSize; x++) {
            // Map to terrain coordinate in global space
            float fx = (chunkX * CHUNK_SIZE) + x * step;
            float fy = (chunkY * CHUNK_SIZE) + y * step;

            int ix = (int)fx;
            int iy = (int)fy;
            int idx = iy * mapSize + ix;

            if (ix < 1 || iy < 1 || ix >= mapSize - 1 || iy >= mapSize - 1) {
                pixels[y * outSize + x] = BLACK;
                continue;
            }

            // Compute world-space position for terrain height and tree placement
            float worldX = (fx - mapSize / 2.0f) * MAP_SCALE;
            float worldZ = (fy - mapSize / 2.0f) * MAP_SCALE;

            // Height gradient calculation
            float hL = heightData[iy * mapSize + (ix - 1)];
            float hR = heightData[iy * mapSize + (ix + 1)];
            float hD = heightData[(iy - 1) * mapSize + ix];
            float hU = heightData[(iy + 1) * mapSize + ix];

            float dhdx = (hR - hL) * heightScale / 2.0f;
            float dhdy = (hU - hD) * heightScale / 2.0f;
            float gradientMag = sqrtf(dhdx * dhdx + dhdy * dhdy);

            // Color analysis (grassy?)
            //Color c = colorData[idx]; //todo: water and snow biomes should use this
            Color c = foliageData[idx];
            bool isInHeightRange = true;
            Model_Type type;
            if (heightData[iy * mapSize + ix] > 0.85f)
            {
                type = GetRandomModelForBiome(BIOME_FOREST_PINE); //TODO: really should be its own thing
            }
            else if (heightData[iy * mapSize + ix] < -0.32)
            {
                type = GetRandomModelForBiome(BIOME_MOUNTAIN_1); //TODO: really should be its own thing
            }
            else if (heightData[iy * mapSize + ix] > 0 && heightData[iy * mapSize + ix] < 0.75f)
            {
                type = GetModelTypeFromColor(c, heightData[iy * mapSize + ix]);
            }
            else
            {
                isInHeightRange = false;
            }
            //TraceLog(LOG_INFO, "color-rgba %d %d %d %d and type = %d", c.r,c.g,c.b,c.a,type);
            bool isFlat = (gradientMag < 0.44f);
            //bool isInHeightRange =  ; //&& heightData[iy * mapSize + ix] < ? TODO: limit how high, also sealevel?
            //
            // Reject points that fall on roads
            int roadX = (int)fx;
            int roadY = (int)fy;
            Color roadPixel = GetImageColor(hardRoadMap, roadX, roadY);
            bool isNotRoad = (roadPixel.r < 250);  // black = road, white = OK .. maybe the opposite?

            pixels[y * outSize + x] = (type > MODEL_NONE && type < MODEL_TOTAL_COUNT && isFlat && isNotRoad && isInHeightRange) ? (Color){type, type, type, 42}: BLACK;
        }
    }

    // --- Position the model in world space ---
    float worldHalfSize = (CHUNK_COUNT * CHUNK_SIZE) / 2.0f;
    // Get image size
    int width = vegImage.width;
    int height = vegImage.height;
    // Compute world-space corner of this chunk
    // float chunkBaseX = cx * CHUNK_SIZE * MAP_SCALE;
    // float chunkBaseZ = cy * CHUNK_SIZE * MAP_SCALE;
    float chunkBaseX = (chunkX * CHUNK_SIZE - worldHalfSize) * MAP_SCALE;
    float chunkBaseZ = (chunkY * CHUNK_SIZE - worldHalfSize) * MAP_SCALE;

    // Allocate memory for tree positions (worst case: 1 tree per pixel)
    StaticGameObject *props = (StaticGameObject *)MemAlloc(sizeof(StaticGameObject) * width * height);
    int propsCounter[MODEL_TOTAL_COUNT] = { 0 }; //todo: do I need this?
    int totalProps = 0;

    for (int y = 0; y < height; y++) {
        bool bobDole = false;
        for (int x = 0; x < width; x++) {
            Color col = pixels[y * width + x];

            if (col.a == 42) {  // 42 here means we found a hit
                unsigned int noise = HashCoords(x, y);
                Model_Type type = (Model_Type)col.r; //for now we can  just use the red channel, but if total models go beyond that, we might need a system that uses mutpli channels
                if ((noise % ASSET_TWEAK_MOD) < ASSET_TWEAK_THRESH) {  // 0.0055% chance to keep — tweak for density
                    // World XZ position
                    float worldX = chunkBaseX + x;
                    float worldZ = chunkBaseZ + y;
                    TraceLog(LOG_INFO, "Chunk (%d,%d), Base(%f,%f), World(%f,%f)", chunkX, chunkY, chunkBaseX, chunkBaseZ, worldX, worldZ);
                    // Get height using terrain function
                    float worldY = GetTerrainHeightFromMeshXZ(chunkModels[chunkX][chunkY].meshes[0], (Vector3){chunkBaseX,0.0f,chunkBaseZ}, worldX, worldZ);
                    //float worldY = 0.0f;
                    //get ratotation and scale
                    float yaw = randfrang(0.0f, PI);                    // 0 to π
                    float pitch = randfrang(-3.4f * DEG2RAD, 3.38f * DEG2RAD);  // ±10° in radians
                    float roll = randfrang(-3.42f * DEG2RAD, 3.41f * DEG2RAD);   // ±10° in radians
                    float scale = randfrang(0.888f, 3.2f);                         // 0.5 to 2.5
                    // Store the tree position
                    props[totalProps++] = (StaticGameObject){type, (Vector3){ worldX, worldY, worldZ}, yaw, pitch, roll, scale};
                    propsCounter[type]++;
                }
                if(totalProps > MAX_PROPS_ALLOWED){bobDole=true;break;}
            }
        }
        if(bobDole){break;}
    }
    SaveTreePositions(chunkX,chunkY,props,totalProps);
    //okay here we go, time to bake the cookies
    //---------------------------------------------------------------------------------------------------------
    for(int i=0; i<MODEL_TOTAL_COUNT; i++)
    {
        ExportBatchTiles(chunkX, chunkY, props, totalProps, propsCounter[i], (Model_Type) i);
    }
    //ding cooies are done!
    //---------------------------------------------------------------------------------------------------------
    MemFree(props);
    props = NULL;  // (optional safety)
    //todo: remove this if it doesnt look cool anymore
    char fname[64];
    snprintf(fname, sizeof(fname), "map/chunk_%02d_%02d/vegetation.png", chunkX, chunkY);
    ExportImage(vegImage, fname);
    UnloadImage(vegImage);
}


///WATER!

#define PATCH_MAX 4096 //orignally I wanted many chunks of water for culling but then we have seams and it doesnt look as nice, so keep thisat 4096
#define MAX_REGION_SIZE (CHUNK_SIZE * CHUNK_SIZE)
#define WATER_TILE_SIZE 16.0f
#define ORIGIN_CHUNK_X 8
#define ORIGIN_CHUNK_Y 8

void ExportOBJMeshSplit(bool *regionMask, float originX, float originZ, int w, int h, int cx, int cy) {
    int maxTiles = w * h;
    int maxQuads = 0;
    for (int i = 0; i < maxTiles; i++) {if (regionMask[i]) {maxQuads++;}}

    int patchCount = (maxQuads + PATCH_MAX - 1) / PATCH_MAX;
    int quadIndex = 0;

    for (int p = 0; p < patchCount; p++) {
        int quadsThisPatch = ((quadIndex + PATCH_MAX) > maxQuads) ? (maxQuads - quadIndex) : PATCH_MAX;

        int numVerts = quadsThisPatch * 4;
        int numTris  = quadsThisPatch * 2;

        float *vertices = (float *)malloc(sizeof(float) * 3 * numVerts);
        float *normals = (float *)malloc(sizeof(float) * 3 * numVerts);
        float *texcoords = (float *)malloc(sizeof(float) * 2 * numVerts);
        unsigned short *indices = (unsigned short *)malloc(sizeof(unsigned short) * 3 * numTris);

        if (!vertices || !normals || !texcoords || !indices) {
            TraceLog(LOG_ERROR, "[Water] Memory allocation failed");
            free(vertices); free(normals); free(texcoords); free(indices);
            return;
        }

        int vi = 0, ti = 0, ii = 0;
        int tilesProcessed = 0;

        for (int y = 0; y < h && tilesProcessed < quadIndex + quadsThisPatch; y++) {
            for (int x = 0; x < w && tilesProcessed < quadIndex + quadsThisPatch; x++) {
                int idx = y * w + x;
                if (!regionMask[idx]) continue;
                if (tilesProcessed < quadIndex) { tilesProcessed++; continue; }
                if (tilesProcessed >= quadIndex + quadsThisPatch) {break;}

                float wx = originX + x * WATER_TILE_SIZE;
                float wz = originZ + y * WATER_TILE_SIZE;
                float wy = 295.0f;

                // Top Left
                vertices[vi*3+0] = wx;
                vertices[vi*3+1] = wy;
                vertices[vi*3+2] = wz;
                texcoords[vi*2+0] = 0.0f;
                texcoords[vi*2+1] = 0.0f;
                normals[vi*3+0] = 0.0f;
                normals[vi*3+1] = 1.0f;
                normals[vi*3+2] = 0.0f;
                vi++;

                // Top Right
                vertices[vi*3+0] = wx + WATER_TILE_SIZE;
                vertices[vi*3+1] = wy;
                vertices[vi*3+2] = wz;
                texcoords[vi*2+0] = 1.0f;
                texcoords[vi*2+1] = 0.0f;
                normals[vi*3+0] = 0.0f;
                normals[vi*3+1] = 1.0f;
                normals[vi*3+2] = 0.0f;
                vi++;

                // Bottom Left
                vertices[vi*3+0] = wx;
                vertices[vi*3+1] = wy;
                vertices[vi*3+2] = wz + WATER_TILE_SIZE;
                texcoords[vi*2+0] = 0.0f;
                texcoords[vi*2+1] = 1.0f;
                normals[vi*3+0] = 0.0f;
                normals[vi*3+1] = 1.0f;
                normals[vi*3+2] = 0.0f;
                vi++;

                // Bottom Right
                vertices[vi*3+0] = wx + WATER_TILE_SIZE;
                vertices[vi*3+1] = wy;
                vertices[vi*3+2] = wz + WATER_TILE_SIZE;
                texcoords[vi*2+0] = 1.0f;
                texcoords[vi*2+1] = 1.0f;
                normals[vi*3+0] = 0.0f;
                normals[vi*3+1] = 1.0f;
                normals[vi*3+2] = 0.0f;
                vi++;

                int base = (vi - 4);
                indices[ii++] = base + 0;
                indices[ii++] = base + 1;
                indices[ii++] = base + 2;
                indices[ii++] = base + 1;
                indices[ii++] = base + 3;
                indices[ii++] = base + 2;

                tilesProcessed++;
            }
            if (tilesProcessed >= quadIndex + quadsThisPatch) {break;}
        }

        Mesh mesh = { 0 };
        mesh.vertexCount = numVerts;
        mesh.triangleCount = numTris;
        mesh.vertices = vertices;
        mesh.normals = normals;
        mesh.texcoords = texcoords;
        mesh.indices = indices;

        char filename[256];
        snprintf(filename, sizeof(filename), "map/chunk_%02d_%02d/water/", cx, cy);
        EnsureDirectoryExists(filename);
        snprintf(filename, sizeof(filename), "map/chunk_%02d_%02d/water/patch_%d.obj", cx, cy, p);
        ExportMesh(mesh, filename);

        FILE *fp = fopen("map/water_manifest.txt", "a");
        if (fp) {
            fprintf(fp, "%d %d %d\n", cx, cy, p);
            fclose(fp);
        }
        quadIndex += quadsThisPatch;
    }
}

// Basic 4-connected flood fill, now records region into regionMap and bounding box
void FloodFillRegion(bool *visited, bool *waterMask, bool *regionMap, int x, int y, int width, int height,
                     int *minX, int *minY, int *maxX, int *maxY) {
    if (x < 0 || x >= width || y < 0 || y >= height) return;
    int idx = y * width + x;
    if (visited[idx] || !waterMask[idx]) return;

    visited[idx] = true;
    regionMap[idx] = true;

    if (x < *minX) *minX = x;
    if (y < *minY) *minY = y;
    if (x > *maxX) *maxX = x;
    if (y > *maxY) *maxY = y;

    FloodFillRegion(visited, waterMask, regionMap, x + 1, y, width, height, minX, minY, maxX, maxY);
    FloodFillRegion(visited, waterMask, regionMap, x - 1, y, width, height, minX, minY, maxX, maxY);
    FloodFillRegion(visited, waterMask, regionMap, x, y + 1, width, height, minX, minY, maxX, maxY);
    FloodFillRegion(visited, waterMask, regionMap, x, y - 1, width, height, minX, minY, maxX, maxY);
}

void CreateWaterPlanes(int chunkX, int chunkY, float *heightData, int mapSize, float heightThreshold) {
    const int size = CHUNK_SIZE;
    const int offsetX = chunkX * size;
    const int offsetY = chunkY * size;
    size_t cells = (size_t)size * (size_t)size;

    bool *waterMask = (bool *)calloc(cells, sizeof(bool));
    bool *visited   = (bool *)calloc(cells, sizeof(bool));
    if (!waterMask || !visited) {
        TraceLog(LOG_ERROR, "CreateWaterPlanes: out of memory (size=%d)", size);
        free(waterMask);
        free(visited);
        return;
    }
    //printf("------WATERGRID (%d,%d)------\n", chunkX, chunkY);
    for (int y = 0; y < size; y++) {
        //printf("WATERGRID ROW - ");
        for (int x = 0; x < size; x++) {
            int globalX = offsetX + x;
            int globalY = offsetY + y;
            float h = heightData[globalY * mapSize + globalX];
            waterMask[y * size + x] = (h < heightThreshold);
            //printf(h<heightThreshold?"1":"0");
        }
        //printf("\n");
    }
    //printf("-----WATERGRID END (%d,%d)------\n", chunkX, chunkY);

    int patchCount = 0;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            int idx = y * size + x;
            if (!visited[idx] && waterMask[idx]) {
                int minX = x, minY = y, maxX = x, maxY = y;
                bool *regionMap = (bool *)calloc(cells, sizeof(bool));

                FloodFillRegion(visited, waterMask, regionMap, x, y, size, size, &minX, &minY, &maxX, &maxY);

                int regionWidth = maxX - minX + 1;
                int regionHeight = maxY - minY + 1;

                if (regionWidth > 0 && regionHeight > 0) {
                    float originX = (chunkX - ORIGIN_CHUNK_X) * 1024; //WATER_TILE_SIZE;
                    float originZ = (chunkY - ORIGIN_CHUNK_Y) * 1024; //WATER_TILE_SIZE;
                    ExportOBJMeshSplit(regionMap, originX, originZ, size,size, chunkX, chunkY);

                    TraceLog(LOG_INFO, "Detected water patch (%d,%d) size %dx%d", chunkX, chunkY, regionWidth, regionHeight);
                    patchCount++;
                }
                free(regionMap);
            }
        }
    }

    free(waterMask);
    free(visited);
}

//--MAIN--
int main(void)
{
    // main character right here
    float *heightData = (float *)MemAlloc(MAP_SIZE * MAP_SIZE * sizeof(float));

    InitWindow(1200, 800, "Perlin Heightmap to Mesh Viewer");
    SetTargetFPS(60);
    SetTraceLogLevel(LOG_INFO);
    //all of the models for static props, for tile batching--------------------------
    InitStaticGameProps(LoadShader(0,0));//default shader, we dont batch the high fi stuff
    // for (int i = 0; i < MODEL_TOTAL_COUNT; i++) {
    //     if (StaticObjectModels[i].meshCount == 0) {
    //         IsModelValid();
    //         TraceLog(LOG_WARNING, "Static model %d (%s) failed to load or is empty", i, GetModelName(i));
    //     }
    // }
    // return -1;
    //-------------------------------------------------------------------------------

    float scale = 4.0f;
    float frequency = 2.0f;
    float lacunarity = 1.0f;
    int octaves = 7;
    int seed = 0;

    Image image = {
        .data = MemAlloc(MAP_SIZE * MAP_SIZE * sizeof(Color)),
        .width = MAP_SIZE,
        .height = MAP_SIZE,
        .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8,
        .mipmaps = 1
    };
    Image colorImage = {
        .data = MemAlloc(MAP_SIZE * MAP_SIZE * sizeof(Color)),
        .width = MAP_SIZE,
        .height = MAP_SIZE,
        .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8,
        .mipmaps = 1
    };
    Image slopeImage = {
        .data = MemAlloc(MAP_SIZE * MAP_SIZE * sizeof(Color)),
        .width = MAP_SIZE,
        .height = MAP_SIZE,
        .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8,
        .mipmaps = 1
    };

    Texture2D texture;
    Texture2D colorTexture;
    GenerateHeightmap(heightData, MAP_SIZE, MAP_SIZE, scale, frequency, octaves, seed, lacunarity);
    RebuildImageFromHeightData(&image, heightData, MAP_SIZE, MAP_SIZE);
    RebuildColorImageFromHeightData(&colorImage, heightData, MAP_SIZE, MAP_SIZE);
    RebuildSlopeImageFromHeightData(&slopeImage, heightData, MAP_SIZE, MAP_SIZE);
    texture = LoadTextureFromImage(image);
    colorTexture = LoadTextureFromImage(colorImage);
    Color *colorData;

    bool isViewing3D = false;
    Camera3D camera = {
        .position = (Vector3){ 512, 80, 512 },
        .target = (Vector3){ 512 + 1, 80, 512 },
        .up = (Vector3){ 0.0f, 1.0f, 0.0f },
        .fovy = 45.0f,
        .projection = CAMERA_PERSPECTIVE
    };

    float cameraYaw = 0.0f;
    float cameraPitch = 0.0f;
    const float moveSpeed = 40.0f;
    const float mouseSensitivity = 0.003f;
    bool doneSave = false;
    DisableCursor(); // hides and locks mouse


    while (!WindowShouldClose())
    {
        if (!isViewing3D) {
            bool dirty=false;
            if (IsKeyPressed(KEY_R)) 
            {
                seed = rand();
                dirty=true;
            }
            if (IsKeyDown(KEY_RIGHT)) {frequency += 0.1f; dirty=true;}
            if (IsKeyDown(KEY_LEFT)) {frequency -= 0.1f; dirty=true;}
            if (IsKeyDown(KEY_UP)) {scale += 0.1f; dirty=true;}
            if (IsKeyDown(KEY_DOWN)) {scale -= 0.1f; dirty=true;}
            if (IsKeyPressed(KEY_O)) {octaves = (octaves % 8) + 1; dirty=true;}
            if (IsKeyPressed(KEY_L)) {lacunarity += 0.1f; dirty=true;}
            if (IsKeyDown(KEY_Z)) {sealevel -= 0.001f; if(sealevel<-0.5f){sealevel=-0.5f;}}
            if (IsKeyDown(KEY_X)) {sealevel += 0.001f; if(sealevel>0.5f){sealevel=0.5f;}}
            // Clamp values
            if (scale < 0.1f) scale = 0.1f;
            if (frequency < 0.1f) frequency = 0.1f;
            if(dirty)
            {
                TraceLog(LOG_INFO, "dirty ... ");
                GenerateHeightmap(heightData, MAP_SIZE, MAP_SIZE, scale, frequency, octaves, seed, lacunarity);
            }
            if (IsKeyPressed(KEY_E)) {
                TraceLog(LOG_INFO, "erosion ... ");
                ApplyErosion(heightData, MAP_SIZE, MAP_SIZE, 5, 0.01f);
            }
            if (IsKeyPressed(KEY_Y)) {
                TraceLog(LOG_INFO, "erosion (hydrolic) ... ");
                ApplyErosionHydraulic(heightData, MAP_SIZE, MAP_SIZE);
            }
            if (IsKeyPressed(KEY_B)) {
                TraceLog(LOG_INFO, "border ... ");
                ApplyBorderFade(heightData, MAP_SIZE, MAP_SIZE, 1.0f, 0.2f, -0.5f);
            }
            if (IsKeyPressed(KEY_H)) {
                TraceLog(LOG_INFO, "h ... ");
                ApplyHeightCurve(heightData, MAP_SIZE, MAP_SIZE, 0.2f, 0.30f, 0.10f); // adjust power to control flattening
            }
            if (IsKeyPressed(KEY_Q)) {
                TraceLog(LOG_INFO, "sig ... ");
                ApplyHeightSigmoidFilter(heightData, MAP_SIZE, MAP_SIZE, 0.8f, 0.5f, 0.6f); // adjust power to control flattening
            }
            if (IsKeyPressed(KEY_S)) {
                TraceLog(LOG_INFO, "smooth ... ");
                ApplySmoothNoiseOverlay(heightData, MAP_SIZE, MAP_SIZE, 0.02f, 8.0f);
            }
            if(IsKeyPressed(KEY_I)) {
                TraceLog(LOG_INFO, "toasty ... ");
                ApplyHeightNegativeFilter(heightData, MAP_SIZE, MAP_SIZE,1);
            }
            if(IsKeyPressed(KEY_N)) {
                TraceLog(LOG_INFO, "grass ... ");
                ApplyHeightAntiSigmoidFilter(heightData, MAP_SIZE, MAP_SIZE, 0.8f, 0.5f, 0.6f);
            }
            if(IsKeyPressed(KEY_W)) {
                TraceLog(LOG_INFO, "sand ... ");
                ApplyHeightAntiSigmoidFilter(heightData, MAP_SIZE, MAP_SIZE, 0.8f, 0.0f, 0.4f);
            }

            
            RebuildImageFromHeightData(&image, heightData, MAP_SIZE, MAP_SIZE);
            RebuildColorImageFromHeightData(&colorImage, heightData, MAP_SIZE, MAP_SIZE);
            RebuildSlopeImageFromHeightData(&slopeImage, heightData, MAP_SIZE, MAP_SIZE);
            UpdateTexture(texture, image.data);


            if (IsKeyPressed(KEY_ENTER)) {
                TraceLog(LOG_INFO, "Road Stuff ...");
                TraceLog(LOG_INFO, "Feature Points for Roads ...");
                // Generate feature points ... for roods!
                int found = 0;
                int stride = 16; // space between sample attempts

                for (int y = stride; y < MAP_SIZE - stride; y += stride) {
                    for (int x = stride; x < MAP_SIZE - stride; x += stride) {
                        int idx = y * CHUNK_SIZE + x;
                        float h = heightData[idx];

                        // Local slope check (less than 15 degrees-ish)
                        float hL = heightData[y * CHUNK_SIZE + (x - 1)];
                        float hR = heightData[y * CHUNK_SIZE + (x + 1)];
                        float hU = heightData[(y + 1) * CHUNK_SIZE + x];
                        float hD = heightData[(y - 1) * CHUNK_SIZE + x];

                        float dhdx = (hR - hL) * HEIGHT_SCALE / 2.0f;
                        float dhdy = (hU - hD) * HEIGHT_SCALE / 2.0f;
                        float slope = sqrtf(dhdx * dhdx + dhdy * dhdy);

                        if (slope > 0.6f) continue; // skip steep regions

                        // Check 3x3 neighborhood for local max
                        bool isMax = true;
                        for (int oy = -1; oy <= 1 && isMax; oy++) {
                            for (int ox = -1; ox <= 1 && isMax; ox++) {
                                if (ox == 0 && oy == 0) continue;
                                int ni = (y + oy) * MAP_SIZE + (x + ox);
                                if (heightData[ni] >= h) isMax = false;
                            }
                        }

                        if (isMax && found < NUM_FEATURE_POINTS) {
                            featurePoints[found++] = (Vector2){ x, y };
                        }
                    }
                }

                TraceLog(LOG_INFO, "found (%d), starting random sampling for features if needed ...?", found);
                if (found < NUM_FEATURE_POINTS / 2) {
                    TraceLog(LOG_WARNING, "Only found %d good points, adding random extras", found);
                    while (found < NUM_FEATURE_POINTS) {
                        featurePoints[found++] = (Vector2){
                            GetRandomValue(0, MAP_SIZE - 1),
                            GetRandomValue(0, MAP_SIZE - 1)
                        };
                    }
                }

                // Create road map image
                roadImage = GenImageColor(ROAD_MAP_SIZE, ROAD_MAP_SIZE, DARKGREEN); // base
                hardRoadMap = GenImageColor(ROAD_MAP_SIZE, ROAD_MAP_SIZE, BLACK); // hard lines, atleast its supposed to be
                
                Color *height_Data = LoadImageColors(image);
                Color *color_data = LoadImageColors(colorImage);//yep, I screwed up the names, and its getting confusing
                GenerateWorleyRoadMap(&roadImage, &hardRoadMap, color_data, height_Data);
                TraceLog(LOG_INFO, "Road map gen - smoothing artifacts ... ");
                ExpandRoadPaths(&hardRoadMap, 1);
                FilterSmallRoadBlobs(&hardRoadMap, 10);  // kill all blobs smaller than 10 pixels
                TraceLog(LOG_INFO, "Road map gen - flattening ... ");
                for (int y = 0; y < MAP_SIZE; y++) {
                    for (int x = 0; x < MAP_SIZE; x++) {
                        int rx = x * ROAD_MAP_SIZE / MAP_SIZE;
                        int ry = y * ROAD_MAP_SIZE / MAP_SIZE;

                        if (rx < 0 || ry < 0 || rx >= ROAD_MAP_SIZE || ry >= ROAD_MAP_SIZE) continue;

                        Color road = GetImageColor(hardRoadMap, rx, ry);
                        if (road.r > 200) {
                            float avg = 0.0f;
                            int count = 0;

                            // Sample local neighborhood to get target flatten height
                            for (int oy = -FLATTEN_RADIUS; oy <= FLATTEN_RADIUS; oy++) {
                                for (int ox = -FLATTEN_RADIUS; ox <= FLATTEN_RADIUS; ox++) {
                                    int nx = x + ox;
                                    int ny = y + oy;
                                    if (nx >= 0 && nx < MAP_SIZE && ny >= 0 && ny < MAP_SIZE) {
                                        avg += heightData[ny * MAP_SIZE + nx];
                                        count++;
                                    }
                                }
                            }

                            if (count > 0) avg /= count;
                            float flattenHeight = avg - FLATTEN_STRENGTH;

                            // Lower all neighbors in the area if they’re higher than the flattenHeight
                            for (int oy = -FLATTEN_RADIUS; oy <= FLATTEN_RADIUS; oy++) {
                                for (int ox = -FLATTEN_RADIUS; ox <= FLATTEN_RADIUS; ox++) {
                                    int nx = x + ox;
                                    int ny = y + oy;
                                    if (nx >= 0 && nx < MAP_SIZE && ny >= 0 && ny < MAP_SIZE) {
                                        float *h = &heightData[ny * MAP_SIZE + nx];
                                        if (*h > flattenHeight) {
                                            *h = Lerp(*h, flattenHeight, FLATTEN_LERP_FACT);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                RebuildImageFromHeightData(&image, heightData, MAP_SIZE, MAP_SIZE);
                colorData = LoadImageColors(image);  // Allocates and returns a Color[], worst named thing ever
                TraceLog(LOG_INFO, "Chunk Stuff ...");
                //models
                for (int cy = 0; cy < CHUNK_COUNT; cy++) {
                    for (int cx = 0; cx < CHUNK_COUNT; cx++) {
                        chunkModels[cx][cy] = GenerateChunkModel(
                            heightData, colorImage, colorData,
                            MAP_SIZE,         // full map size
                            cx, cy,           // chunk coordinates
                            CHUNK_SIZE,       // chunk size
                            HEIGHT_SCALE      // vertical exaggeration
                        );
                    }
                }
                UpdateTexture(colorTexture, colorImage.data);
                isViewing3D = true;
                DisableCursor();
            }
        } else {
            UpdateCamera(&camera, CAMERA_FIRST_PERSON);

            if (IsKeyPressed(KEY_BACKSPACE)) {
                for (int cy = 0; cy < CHUNK_COUNT; cy++) {
                    for (int cx = 0; cx < CHUNK_COUNT; cx++) {
                        UnloadModel(chunkModels[cx][cy]);
                    }
                }
                isViewing3D = false;
                EnableCursor();
            }
            if (IsKeyPressed(KEY_P))
            {
                EnableCursor();
                //for now, I dont want to clamp, to see if the seams and artifacts are better or worse or the same
                //what I think is happening is screwing with the vertices causes problems with normals, and then distant rendering is a problem
                //as Im writing this, Im not sure tho as I cant test yest (map gen takes a while and I have other things first)
                //TraceLog(LOG_INFO, "Clamping LOD edges ...");
                // ClampMeshEdges(chunkModels32, 33);//todo, should i remove these clamp functions, or change, them, I still see some very small seams?
                // ClampMeshEdges(chunkModels16, 17);
                // ClampMeshEdges(chunkModels8, 9);
                
                TraceLog(LOG_INFO, "road stuff again ... (and in game map image)");
                Color *colorData = LoadImageColors(colorImage);
                Image inGameMap = ImageCopy(colorImage);
                Image foliage = GenImageColor(MAP_SIZE, MAP_SIZE, BLACK);
                GenerateFoliageMap(&foliage, MAP_SIZE, MAP_SIZE, 2.0f, 3.0f, 4, rand(), 1.4f);
                ImageResize(&inGameMap,128,128);
                remove("map/manifest.txt");
                remove("map/water_manifest.txt");
                ExportImage(foliage, "map/foliage_map.png");
                ExportImage(roadImage, "map/road_map.png");
                ExportImage(hardRoadMap, "map/hard_road_map.png");
                ExportImage(inGameMap, "map/elevation_color_map.png");
                ExportImage(image, "map/map_height.png");
                ExportImage(slopeImage, "map/map_slope.png");

                TraceLog(LOG_INFO, "Exporting all chunks...");
                for (int cy = 0; cy < CHUNK_COUNT; cy++) {
                    for (int cx = 0; cx < CHUNK_COUNT; cx++) {
                        //check directory first
                        TraceLog(LOG_INFO, "Checking Directory (%d,%d)...", cx, cy);
                        char fnameDir[64];
                        snprintf(fnameDir, sizeof(fnameDir), "map/chunk_%02d_%02d", cx, cy);
                        EnsureDirectoryExists(fnameDir);

                        TraceLog(LOG_INFO, "Exporting chunk (%d,%d)...", cx, cy);
                        int chunkSize = CHUNK_SIZE + 1;  // 65
                        // ...
                        for (int y = 0; y < chunkSize; y++) {
                            for (int x = 0; x < chunkSize; x++) {
                                int globalX = cx*CHUNK_SIZE + x;
                                int globalY = cy*CHUNK_SIZE + y;
                                // <-- add these two clamps:
                                if (globalX >= MAP_SIZE) globalX = MAP_SIZE - 1;
                                if (globalY >= MAP_SIZE) globalY = MAP_SIZE - 1;

                                int srcIndex = globalY * MAP_SIZE + globalX;   // now always in-bounds
                                int dstIndex = y * chunkSize + x;
                                // ... copy height/color/slope into per-chunk pixels
                            }
                        }

                        Image heightImage = GenImageColor(chunkSize, chunkSize, BLACK);
                        Image colorImage2 = GenImageColor(chunkSize, chunkSize, BLACK);
                        Image slopeImage2 = GenImageColor(chunkSize, chunkSize, BLACK);

                        Color *heightPixels = (Color *)heightImage.data;
                        Color *colorPixels = (Color *)colorImage2.data;
                        Color *colorData = (Color *)colorImage.data;
                        Color *slopeData = (Color *)slopeImage.data;
                        Color *slopePixels = (Color *)slopeImage2.data;

                        for (int y = 0; y < chunkSize; y++) {
                            for (int x = 0; x < chunkSize; x++) {
                                int globalX = cx * CHUNK_SIZE + x;
                                int globalY = cy * CHUNK_SIZE + y;
                                int srcIndex = globalY * MAP_SIZE + globalX;
                                int dstIndex = y * chunkSize + x;

                                // Height to grayscale
                                if (srcIndex >= (MAP_SIZE * MAP_SIZE)) { continue; }
                                float h = heightData[srcIndex];
                                unsigned char gray = (unsigned char)((h + 1.0f) * 127.5f); // Normalize -1..1 to 0..255
                                heightPixels[dstIndex] = (Color){gray, gray, gray, 255};

                                // Color already provided
                                colorPixels[dstIndex] = colorData[srcIndex];
                                slopePixels[dstIndex] = slopeData[srcIndex];
                            }
                        }
                        // Perlin vegetation noise (scale if needed)
                        TraceLog(LOG_INFO, "vegetation (%d,%d)...", cx, cy);
                        SaveChunkVegetationImage(cx, cy, heightData, colorData, foliage.data, MAP_SIZE, HEIGHT_SCALE);
                        TraceLog(LOG_INFO, "water (%d,%d)...", cx, cy);
                        CreateWaterPlanes(cx, cy, heightData, MAP_SIZE, sealevel);
                        char fnameHeight[64];
                        char fnameColor[64];
                        char fnameSlope[64];
                        char fnameSlopeBig[64];
                        char fnameHeight64[64];

                        snprintf(fnameHeight, sizeof(fnameHeight), "map/chunk_%02d_%02d/height.png", cx, cy);
                        snprintf(fnameColor, sizeof(fnameColor), "map/chunk_%02d_%02d/color.png", cx, cy);
                        snprintf(fnameSlope, sizeof(fnameSlope), "map/chunk_%02d_%02d/slope.png", cx, cy);
                        snprintf(fnameSlopeBig, sizeof(fnameSlopeBig), "map/chunk_%02d_%02d/slope_big.png", cx, cy);
                        snprintf(fnameHeight64, sizeof(fnameHeight64), "map/chunk_%02d_%02d/height64.png", cx, cy);

                        // Rebuild mesh for this chunk using height and color image
                        Texture2D textureColors = LoadTextureFromImage(colorImage);
                        Model model = LoadModelFromMesh(chunkModels[cx][cy].meshes[0]);
                        model.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture = textureColors;

                        ImageResize(&colorImage2, 64, 64); // Resize to power-of-two dimensions
                        ImageResize(&slopeImage2, 64, 64); // Resize to power-of-two dimensions
                        Image img = {
                            .data = colorImage2.data,
                            .width = 64,
                            .height = 64,
                            .mipmaps = 1,
                            .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8
                        };
                        Image img2 = {
                            .data = slopeImage2.data,
                            .width = 64,
                            .height = 64,
                            .mipmaps = 1,
                            .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8
                        };
                        Image img3 = ImageCopy(heightImage);
                        ImageResize(&img3,64,64);
                        ExportImage(img3, fnameHeight64);
                        //roads handle - this sets the colors for the textures
                        for(int y=0; y<img.height; y++)
                        {
                            for(int x=0; x<img.width; x++)
                            {
                                int globalX = cx * CHUNK_SIZE + x;
                                int globalY = cy * CHUNK_SIZE + y;
                                Color tp = GetImageColor(hardRoadMap, globalX, globalY);
                                //Color roadPixel = GetImageColor(roadImage, globalX, globalY);
                                Color imgPixel = GetImageColor(img, x, y);
                                Color img2Pixel = GetImageColor(img2, x, y);
                                if (tp.r > 200) {
                                    //average terrain color with road
                                    Color roadColor = (Color){ 100, 80, 50, 255 }; // visible dirt road brown
                                    Color c1 = AverageColor(roadColor, imgPixel);
                                    Color c2 = AverageColor(roadColor, img2Pixel);
                                    ImageDrawPixel(&img, x, y, c1);
                                    ImageDrawPixel(&img2, x, y, c2);
                                }
                            }
                        }
                        //models gen
                        char fnameObj[64];
                        char fnameObj32[64];
                        char fnameObj16[64];
                        char fnameObj8[64];
                        snprintf(fnameObj, sizeof(fnameObj), "map/chunk_%02d_%02d/64.obj", cx, cy);
                        snprintf(fnameObj32, sizeof(fnameObj32), "map/chunk_%02d_%02d/32.obj", cx, cy);
                        snprintf(fnameObj16, sizeof(fnameObj16), "map/chunk_%02d_%02d/16.obj", cx, cy);
                        snprintf(fnameObj8, sizeof(fnameObj8), "map/chunk_%02d_%02d/8.obj", cx, cy);
                        ExportMesh(chunkModels[cx][cy].meshes[0], fnameObj);
                        ExportMesh(chunkModels32[cx][cy].meshes[0], fnameObj32);
                        ExportMesh(chunkModels16[cx][cy].meshes[0], fnameObj16);
                        ExportMesh(chunkModels8[cx][cy].meshes[0], fnameObj8);

                        UnloadModel(model); // also unloads mesh internally
                        UnloadTexture(textureColors);
                        ExportImage(heightImage, fnameHeight);
                        //ExportImage(img, fnameColor);
                        //ExportImage(img2, fnameSlope);
                        //big colors
                        Image upscaled = GenImageColor(UPSCALED_TEXTURE_SIZE, UPSCALED_TEXTURE_SIZE, BLACK);
                        for (int y = 0; y < UPSCALED_TEXTURE_SIZE; y++) {
                            for (int x = 0; x < UPSCALED_TEXTURE_SIZE; x++) {
                                // Get source pixel (mapped to 64x64)
                                int srcX = (x * (img.width)) / (UPSCALED_TEXTURE_SIZE );
                                int srcY = (y * (img.height)) / (UPSCALED_TEXTURE_SIZE );


                                Color base = GetImageColor(img, srcX, srcY);

                                // Optional: jitter brightness slightly to reduce banding
                                int brightnessOffset = rand() % 10 - 5;  // Range -5 to +4
                                base.r = (unsigned char)Clamp(base.r + brightnessOffset, 0, 255);
                                base.g = (unsigned char)Clamp(base.g + brightnessOffset, 0, 255);
                                base.b = (unsigned char)Clamp(base.b + brightnessOffset, 0, 255);

                                ImageDrawPixel(&upscaled, x, y, base);
                            }
                        }

                        
                        char outName[256];
                        snprintf(outName, sizeof(outName), "map/chunk_%02d_%02d/color_big.png", cx, cy);
                        //ExportImage(upscaled, outName);
                        //big slope
                        Image upscaled2 = GenImageColor(UPSCALED_TEXTURE_SIZE, UPSCALED_TEXTURE_SIZE, BLACK);
                        for (int y = 0; y < UPSCALED_TEXTURE_SIZE; y++) {
                            for (int x = 0; x < UPSCALED_TEXTURE_SIZE; x++) {
                                // Get source pixel (mapped to 64x64)
                                int srcX = (x * (img2.width)) / (UPSCALED_TEXTURE_SIZE );
                                int srcY = (y * (img2.height)) / (UPSCALED_TEXTURE_SIZE );

                                Color base = GetImageColor(img2, srcX, srcY);

                                // Optional: jitter brightness slightly to reduce banding
                                int brightnessOffset = rand() % 10 - 5;  // Range -5 to +4
                                base.r = (unsigned char)Clamp(base.r + brightnessOffset, 0, 255);
                                base.g = (unsigned char)Clamp(base.g + brightnessOffset, 0, 255);
                                base.b = (unsigned char)Clamp(base.b + brightnessOffset, 0, 255);

                                ImageDrawPixel(&upscaled2, x, y, base);
                            }
                        }
                        //
                        Image upscaled3 = GenImageColor(UPSCALED_TEXTURE_SIZE, UPSCALED_TEXTURE_SIZE, BLACK);
                        for (int y = 0; y < UPSCALED_TEXTURE_SIZE; y++) {
                            for (int x = 0; x < UPSCALED_TEXTURE_SIZE; x++) {
                                // Get source pixel (mapped to 64x64)
                                int srcX = (x * (img3.width)) / (UPSCALED_TEXTURE_SIZE );
                                int srcY = (y * (img3.height)) / (UPSCALED_TEXTURE_SIZE );

                                Color base = GetImageColor(img3, srcX, srcY);

                                // Optional: jitter brightness slightly to reduce banding
                                int brightnessOffset = rand() % 10 - 5;  // Range -5 to +4
                                base.r = (unsigned char)Clamp(base.r + brightnessOffset, 0, 255);
                                base.g = (unsigned char)Clamp(base.g + brightnessOffset, 0, 255);
                                base.b = (unsigned char)Clamp(base.b + brightnessOffset, 0, 255);

                                ImageDrawPixel(&upscaled3, x, y, base);
                            }
                        }
                        //ExportImage(upscaled2, fnameSlopeBig);

                        Image average = AverageImages(img3,AverageImages(img,img2));
                        Image averageBig = AverageImages(upscaled3,AverageImages(upscaled,upscaled2));//here we are still 1024
                        // ImageResize(&upscaled2, 128, 128); //todo: remove these if not needed
                        // ImageResize(&upscaled, 128, 128);

                        char avgName[256];
                        char avgBigName[256];
                        char avgFullName[256];
                        char avgDamnName[256];
                        snprintf(avgName, sizeof(avgName), "map/chunk_%02d_%02d/avg.png", cx, cy);
                        snprintf(avgBigName, sizeof(avgBigName), "map/chunk_%02d_%02d/avg_big.png", cx, cy);
                        snprintf(avgFullName, sizeof(avgFullName), "map/chunk_%02d_%02d/avg_full.png", cx, cy);
                        snprintf(avgDamnName, sizeof(avgDamnName), "map/chunk_%02d_%02d/avg_damn.png", cx, cy);
                        ExportImage(average, avgName);//far away we can cheat and just use the 64 which is small and very pixely
                        //damn!
                        TraceLog(LOG_INFO, "song2");
                        Image damn = UpscaleImageBilinear(averageBig, 2057, 2057);//damn! (actually the full size now but didnt want to swtich all the variable names)
                        ImageResize(&damn, 1024, 1024);
                        
                        ExportImage(damn, avgDamnName); //damn!
                        ImageResize(&damn, 512, 512);//now we are 512 for full

                        ExportImage(damn, avgFullName);
                        ImageResize(&damn, 256, 256);//256 for big
                        ExportImage(damn, avgBigName);

                        UnloadImage(damn); //beaver? DAMN!
                        UnloadImage(average);
                        UnloadImage(averageBig);
                        UnloadImage(upscaled2);
                        UnloadImage(upscaled);
                        UnloadImage(heightImage);
                        UnloadImage(colorImage2);
                        UnloadImage(slopeImage2);
                    }
                }
                TraceLog(LOG_INFO, "Done exporting.");
                doneSave = true;
                CloseWindow();
            }
        }
        if(!doneSave) //atttempt to prevent seg faults on close window
        {
            // Get delta time
            float dt = GetFrameTime();

            // Mouse delta
            Vector2 mouseDelta = Vector2Negate(GetMouseDelta());
            cameraYaw -= mouseDelta.x * mouseSensitivity;
            cameraPitch -= mouseDelta.y * mouseSensitivity;
            cameraPitch = Clamp(cameraPitch, -89.0f * DEG2RAD, 89.0f * DEG2RAD);

            // Direction vector from yaw/pitch
            Vector3 forward = {
                cosf(cameraPitch) * cosf(cameraYaw),
                sinf(cameraPitch),
                cosf(cameraPitch) * sinf(cameraYaw)
            };
            forward = Vector3Normalize(forward);

            // Right vector
            Vector3 right = Vector3Normalize(Vector3CrossProduct(forward, camera.up));

            // Movement
            Vector3 movement = { 0 };
            if (IsKeyDown(KEY_W)) movement = Vector3Add(movement, forward);
            if (IsKeyDown(KEY_S)) movement = Vector3Subtract(movement, forward);
            if (IsKeyDown(KEY_D)) movement = Vector3Add(movement, right);
            if (IsKeyDown(KEY_A)) movement = Vector3Subtract(movement, right);
            if (IsKeyDown(KEY_SPACE)) movement.y += 1;
            if (IsKeyDown(KEY_LEFT_CONTROL)) movement.y -= 1;

            if (Vector3Length(movement) > 0.0f) {
                movement = Vector3Normalize(movement);
                movement = Vector3Scale(movement, moveSpeed * dt);
                camera.position = Vector3Add(camera.position, movement);
            }

            // Update target to look direction
            camera.target = Vector3Add(camera.position, forward);
        }

        BeginDrawing();
        ClearBackground(BLACK);

        if (!isViewing3D) {
            DrawText(TextFormat("Scale: %.2f | Frequency: %.2f | Octaves: %d | Seed: %d| Lac: %.2f | sea: %.4f", scale, frequency, octaves, seed, lacunarity, sealevel), 10, 10, 20, RAYWHITE);
            DrawText("Use arrow keys | O = Octaves | R = Reseed | Enter = View 3D Mesh | L=Lac", 10, 40, 20, GRAY);
            DrawText("E = Apply Erosion", 10, 70, 20, GRAY);
            DrawText("B = Apply Border Gradient (fade to edges)", 10, 100, 20, GRAY);
            DrawText("H = Smooth Flat ground", 10, 130, 20, GRAY);
            DrawText("Q = Smooth Sigmoid Flat ground", 10, 160, 20, GRAY);
            DrawText("S = Smooth", 10, 190, 20, GRAY);
            DrawText("I = Invert", 10, 220, 20, GRAY);
            DrawText("N = Anit-Sig (Grass)", 10, 250, 20, DARKGREEN);
            DrawText("W = Anit-Sig (Sand)", 10, 280, 20, YELLOW);
            DrawText("y = Droplet Erosion", 10, 310, 20, BLUE);
            DrawTextureEx(texture, (Vector2){ 220, 220 }, 0.0f, MAP_SIZE_SCALE, WHITE);
        } else {
            BeginMode3D(camera);
            for (int cy = 0; cy < CHUNK_COUNT; cy++) {
                for (int cx = 0; cx < CHUNK_COUNT; cx++) {
                    Vector3 pos = {
                        .x = cx * CHUNK_SIZE,
                        .y = 0,
                        .z = cy * CHUNK_SIZE
                    };
                    DrawModel(chunkModels[cx][cy], pos, 1.0f, WHITE);
                }
            }
            DrawGrid(10, 10.0f);
            EndMode3D();
            DrawText("Backspace = Return to Heightmap Editor", 10, 10, 20, RAYWHITE);
            DrawText("P = Save Map And Exit", 10, 40, 20, RAYWHITE);
        }
        EndDrawing();
    }
            
    if (isViewing3D)
    {
        for (int cy = 0; cy < CHUNK_COUNT; cy++) {
            for (int cx = 0; cx < CHUNK_COUNT; cx++) {
                UnloadModel(chunkModels[cx][cy]);
            }
        }
    }

    UnloadTexture(texture);
    UnloadImage(image);
    //MemFree(heightData);
    CloseWindow();

    return 0;
}
