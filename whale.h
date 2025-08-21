#ifndef WHALE_H
#define WHALE_H

#include "raylib.h"
#include "raymath.h"
#include <stdio.h>

//bones in the whale indexes
typedef enum {
    WHALE_BONE_TAIL = 0,  // pick a new goal near home
    WHALE_BONE_AB,    // swim gently toward goal (tail up/down)
    WHALE_BONE_CHEST,      // barrel roll a few spins, then re-plan
    WHALE_BONE_HEAD,      // quick yaw + pitch-down pulse
    WHALE_BONE_MOUTH,      // descend to bottomY
    WHALE_BONE_LEFTFIN,          // accelerate up to surfaceY, roll to back, then re-plan
    WHALE_BONE_RIGHTFIN,
} WhaleBones;
// --------- States ----------
// //potential paths for whale state//
// always *** SWIM_PLAN -> SWIM_CRUISE -[once we are close enough to goal state]> (SPIN_ROLL, TURN_DIVE, GENTLE_BREACH_SWIM) -> SWIM_PLAN
// *SPIN_ROLL -> SWIM_PLAN
// *TURN_DIVE -> SWIM_DIVE -> TURN_ASCEND -> SPIN_ROLL -> SWIM_ASCEND -> BREACH -> BREACH_END -> SWIM_PLAN
// *GENTLE_BREACH_SWIM -> GENTLE_BREACH -> BREACH_BLOW -> SWIM_PLAN
// *GENTLE_BREACH_SWIM -> GENTLE_BREACH -> TAIL_SLAP -> SWIM_PLAN
// at the end of cruise state, SPIN_ROLL, TURN_DIVE, GENTLE_BREACH_SWIM are chosen at random with an equal chance
// at the end of GENTLE_BREACH, BREACH_BLOW or TAIL_SLAP is chosen randomly, and picks up the middle to
//      the end of the animation for the gentle breach so things move smoothly and the correct action (blow/tailslap) happens
typedef enum {
    SWIM_PLAN = 0,  // pick a new goal near home
    SWIM_CRUISE,    // swim gently toward goal (tail up/down)
    SPIN_ROLL,      // barrel roll under water then undo
    SPIN_ROLL_UNDO,      // barrel roll under water to undo then re-plan
    TURN_DIVE,      // HARD BREACH step 1 -> begin to spin down and move tail and ab so the spin makes sense visually
    SWIM_DIVE,      // HARD BREACH step 2 -> un-curl tail and ab and begin to descend just like cruise, but facing down
    TURN_ASCEND,      // HARD BREACH step 3 -> once we hit close enough to the bottom y, gently turn to ascend
    SWIM_ASCEND,      // HARD BREACH step 4 -> ascend at an increasingly rapid pace, tail movements should increase in frequency hear a bit also so visually the speed up makes sense
    BREACH,          // HARD BREACH step 5 -> once we have ascended to slightly past the surface y, water line, burst up, and randomly sort of twist and fall to the side or backward, should be very dynamic
    BREACH_END,          // HARD BREACH step 6 -> gently correct the whales rotations and then replan
    GENTLE_BREACH_SWIM, //GENTLE_BREACH_SWIM-> GENTLE_BREACH
    GENTLE_BREACH, //GENTLE_BREACH
} AnimState;

typedef struct {
    // Model + proc anim
    Model model;
    Texture2D tex;
    ModelAnimation proc;   // 1-frame pose

    Vector3 pos;
    float yaw, pitch, roll;   // world degrees
    float speed;              // units/sec

    // Draw config
    float xFixDeg;
    float yFixPos;

    // Home/goal & FSM
    Vector3 home;
    Vector3 goal;
    AnimState state;
    float stateTime;          // seconds in current state
    int   spinTarget;         // how many full rolls to do
    float spinAccum;          // how much roll we’ve done (degrees)
    float turnYawDelta;       // planned yaw change for TURN_DIVE
    float verticalBoost;      // internal accel-ish for BREACH ascent

    // Tunables
    float homeRadius;   // how far to pick goals from home
    float arriveRadius; // distance considered “arrived” at goal
    float bottomY;      // sea floor
    float surfaceY;     // water surface

    //for breach
    int   breachApex;           // 0 until we pass the apex
    float descendTimer;         // seconds since apex
    Vector3 breachEulerStart;   // yaw/pitch/roll at BREACH entry
    Vector3 breachEulerTarget;  // where we want to end up while falling
} Whale;

// --------- Small helpers ----------
static float AngleWrapDeg(float a) { while (a > 180) a -= 360; while (a < -180) a += 360; return a; }
static float AngleLerpDeg(float a, float b, float k) { float d = AngleWrapDeg(b - a); return a + d * k; }
static float Clamp01(float x) { return (x < 0) ? 0 : (x > 1 ? 1 : x); }
static float SmoothStep(float t) { t = Clamp01(t); return t * t * (3.0f - 2.0f * t); }
static float Frand(float a, float b) { return a + (b - a) * ((float)GetRandomValue(0, 1000000) / 1000000.0f); }
//helpers
static void PoseResetToBind(const Model* m, ModelAnimation* p) {
    for (int b = 0; b < p->boneCount; b++) { p->framePoses[0][b] = m->bindPose[b]; }
}
static void SetFromBindPlusEuler(const Model* m, ModelAnimation* p, int idx, float ex, float ey, float ez) {
    if (idx < 0) return;
    Quaternion dq = QuaternionFromEuler(ex, ey, ez);
    p->framePoses[0][idx].rotation = QuaternionMultiply(dq, m->bindPose[idx].rotation);
}

// BuildWorldQuat (replace the body)
//static Quaternion BuildWorldQuat(const Whale* A) {
//    Quaternion qWorld = QuaternionFromEuler(DEG2RAD * A->pitch,DEG2RAD * A->yaw,DEG2RAD * A->roll);
//    Quaternion qFix = QuaternionFromAxisAngle((Vector3) { 1, 0, 0 },DEG2RAD* A->xFixDeg);
//    // Apply FIX (local) then WORLD: qFinal = qWorld * qFix
//    return QuaternionMultiply(qWorld, qFix);
//    //return qWorld;
//}
// BuildWorldQuat (drop-in replacement)
static Quaternion BuildWorldQuat(const Whale* A) {
    // Local fix first (applied closest to the mesh)
    Quaternion qFix = QuaternionFromAxisAngle((Vector3) { 1, 0, 0 }, DEG2RAD* A->xFixDeg);

    // Compose world-space yaw then local pitch
    Quaternion qYaw = QuaternionFromAxisAngle((Vector3) { 0, 1, 0 }, DEG2RAD* A->yaw);
    Quaternion qPitch = QuaternionFromAxisAngle((Vector3) { 1, 0, 0 }, DEG2RAD* A->pitch);

    // During gentle-breach chain, roll around model X; otherwise roll around model Z
    bool rollOnX = (A->state == GENTLE_BREACH_SWIM) || (A->state == GENTLE_BREACH);

    Quaternion qRoll = rollOnX
        ? QuaternionFromAxisAngle((Vector3) { 1, 0, 0 }, DEG2RAD* A->roll)
        : QuaternionFromAxisAngle((Vector3) { 0, 0, 1 }, DEG2RAD* A->roll);

    // Final = yaw → pitch → (roll on chosen axis) → fix
    Quaternion qWorld = QuaternionMultiply(QuaternionMultiply(qYaw, qPitch), qRoll);
    return QuaternionMultiply(qWorld, qFix);
}

// ----- Content loading -----
static bool LoadWhale(Whale* A) {
    A->model = LoadModel("models/whale.glb");
    A->tex = LoadTexture("textures/whale.png");
    if (A->tex.id) SetMaterialTexture(&A->model.materials[0], MATERIAL_MAP_ALBEDO, A->tex);

    // 1-frame procedural animation
    A->proc.boneCount = A->model.boneCount;
    A->proc.bones = A->model.bones;
    A->proc.frameCount = 1;
    A->proc.framePoses = MemAlloc(sizeof(Transform*) * 1);
    A->proc.framePoses[0] = MemAlloc(sizeof(Transform) * A->proc.boneCount);
    PoseResetToBind(&A->model, &A->proc);
    return true;
}

static void InitWhale(Whale* A, Vector3 home, float bottomY, float surfaceY) {
    A->pos = home;
    A->yaw = 0.0f; A->pitch = 0.0f; A->roll = 0.0f;
    A->speed = 1.4f;       // gentle default
    A->xFixDeg = -90.0f;   // exporter fix
    A->yFixPos = 2.2f;   // exporter fix

    // Tunable params
    A->home = home;
    A->homeRadius = 14.0f;
    A->arriveRadius = 3.21f;
    A->bottomY = bottomY;     // “floor” (settable)
    A->surfaceY = surfaceY;    // “surface” (settable)

    A->goal = A->home;
    A->state = SWIM_PLAN;
    A->stateTime = 0.0f;
    A->spinTarget = 0;
    A->spinAccum = 0.0f;
    A->turnYawDelta = 0.0f;
    A->verticalBoost = 0.0f;

    DisableCursor();
}

// ---------- FSM helpers (Fish State Management)----------
// ------------ FSM helpers you can tweak ------------
static const char* StateName(AnimState s) {
    switch (s) {
    case SWIM_PLAN:          return "SWIM_PLAN";
    case SWIM_CRUISE:        return "SWIM_CRUISE";
    case SPIN_ROLL:          return "SPIN_ROLL";
    case SPIN_ROLL_UNDO:     return "SPIN_ROLL_UNDO";
    case TURN_DIVE:          return "TURN_DIVE";
    case SWIM_DIVE:          return "SWIM_DIVE";
    case TURN_ASCEND:        return "TURN_ASCEND";
    case SWIM_ASCEND:        return "SWIM_ASCEND";
    case BREACH:             return "BREACH";
    case BREACH_END:         return "BREACH_END";
    case GENTLE_BREACH_SWIM: return "GENTLE_BREACH_SWIM";
    case GENTLE_BREACH:      return "GENTLE_BREACH";
    default:                 return "UNKNOWN";
    }
}

static inline float Dist3(Vector3 a, Vector3 b) { return Vector3Distance(a, b); }
static inline bool Near(Vector3 a, Vector3 b, float r) { return Dist3(a, b) <= r; }

// FacePoint: smoothly orient whale so +Z looks at target (no effect on position)
static void FacePoint(Whale* A, Vector3 target, float dt, float yawLerp, float pitchLerp) {
    Vector3 to = Vector3Subtract(target, A->pos);
    float horiz = sqrtf(to.x * to.x + to.z * to.z) + 1e-6f;

    // With +Z as forward: standard yaw/pitch
    float yawTarget = RAD2DEG * atan2f(to.x, to.z);
    float pitchTarget = RAD2DEG * -atan2f(to.y, horiz);  // positive = nose up toward higher target

    A->yaw = AngleLerpDeg(A->yaw, yawTarget, yawLerp);
    A->pitch = AngleLerpDeg(A->pitch, pitchTarget, pitchLerp);
    A->roll = AngleLerpDeg(A->roll, 0.0f, 0.08f);  // auto-level roll
}
// Translate-only step toward target (rotation independent)
static void MoveToward(Whale* A, Vector3 target, float dt) {
    Vector3 to = Vector3Subtract(target, A->pos);
    float d = Vector3Length(to);
    if (d < 1e-6f) return;
    float maxStep = A->speed * dt * 10.0; //with new scale and framerate ...?
    if (d <= maxStep) { A->pos = target; return; }
    A->pos = Vector3Add(A->pos, Vector3Scale(to, maxStep / d));
}

// Your nice Cruise bones, factored so FSM can call it
static void ApplyCruiseBones(Whale* A, float t) {
    float s2 = sinf(t * 2.0f), s3 = sinf(t * 3.0f);
    SetFromBindPlusEuler(&A->model, &A->proc, WHALE_BONE_TAIL, DEG2RAD * 10.0f * s2, 0, 0);
    SetFromBindPlusEuler(&A->model, &A->proc, WHALE_BONE_AB, DEG2RAD * 4.0f * s2, 0, 0);
    SetFromBindPlusEuler(&A->model, &A->proc, WHALE_BONE_CHEST, DEG2RAD * 2.0f * s2, 0, 0);
    SetFromBindPlusEuler(&A->model, &A->proc, WHALE_BONE_HEAD, DEG2RAD * 1.0f * s2, 0, 0);
    SetFromBindPlusEuler(&A->model, &A->proc, WHALE_BONE_LEFTFIN, 0, 0, DEG2RAD * 5.0f * s3);
    SetFromBindPlusEuler(&A->model, &A->proc, WHALE_BONE_RIGHTFIN, 0, 0, -DEG2RAD * 5.0f * s3);
}
// Softer swim beats (dive/ascend variants)
static void ApplyDiveBones(Whale* A, float t, float tailDeg) {
    float s2 = sinf(t * 2.0f);
    SetFromBindPlusEuler(&A->model, &A->proc, WHALE_BONE_TAIL, DEG2RAD * tailDeg * s2, 0, 0);
    SetFromBindPlusEuler(&A->model, &A->proc, WHALE_BONE_AB, DEG2RAD * (tailDeg * 0.4f) * s2, 0, 0);
}
static void ApplyBreachBones(Whale* A, float t, float tailDeg) {
    float s3 = sinf(t * 3.0f);
    SetFromBindPlusEuler(&A->model, &A->proc, WHALE_BONE_TAIL, DEG2RAD * tailDeg * s3, 0, 0);
    SetFromBindPlusEuler(&A->model, &A->proc, WHALE_BONE_AB, DEG2RAD * (tailDeg * 0.5f) * s3, 0, 0);
    SetFromBindPlusEuler(&A->model, &A->proc, WHALE_BONE_CHEST, DEG2RAD * (tailDeg * 0.25f) * s3, 0, 0);
    SetFromBindPlusEuler(&A->model, &A->proc, WHALE_BONE_HEAD, DEG2RAD * 0.78f * s3, 0, 0);
    SetFromBindPlusEuler(&A->model, &A->proc, WHALE_BONE_LEFTFIN, 0, 0, DEG2RAD * 5.0f * s3);
    SetFromBindPlusEuler(&A->model, &A->proc, WHALE_BONE_RIGHTFIN, 0, 0, -DEG2RAD * 5.0f * s3);
}
static void ApplyTailSlapBones(Whale* A, float t, float tailDeg) {
    float s3 = sinf(t * 3.0f);
    SetFromBindPlusEuler(&A->model, &A->proc, WHALE_BONE_TAIL, DEG2RAD * tailDeg * s3, 0, 0);
    SetFromBindPlusEuler(&A->model, &A->proc, WHALE_BONE_AB, DEG2RAD * (tailDeg * 0.5f) * s3, 0, 0);
}

// Pick a new far-ish horizontal goal around home
static Vector3 PickGoalAroundHome(Whale* A) {
    float ang = Frand(0.0f, 2.0f * PI);
    float r = Frand(A->homeRadius * 0.4f, A->homeRadius);
    float gy = Frand(A->home.y - 1.0f, A->home.y + 1.0f);
    return (Vector3) { A->home.x + r * cosf(ang), gy, A->home.z + r * sinf(ang) };
}

// Decide next after Cruise (equal chance)
static void DecideAfterCruise(Whale* A) {
    int r = GetRandomValue(0, 5); // 0..2
    if (r < 3) { A->state = SPIN_ROLL; } //0,1,2 -> gentle rolls, changes direction but I am okay with that for now, looks okay
    else if (r == 3) { A->state = TURN_DIVE; } //3 -> breach
    else { A->state = GENTLE_BREACH_SWIM; } //4,5 -> gentle breach, blow or tail slap
    //A->state = GENTLE_BREACH_SWIM;//manually set what we are testing
    A->stateTime = 0.0f;
}

// Enter helper preserving your existing init behavior
static void EnterState(Whale* A, AnimState s) {
    A->state = s; A->stateTime = 0.0f;
    switch (s) {
    case SWIM_PLAN:       A->goal = PickGoalAroundHome(A); A->speed = 1.3f; break;
    case SWIM_CRUISE:     A->speed = 1.5f; break;
    case SPIN_ROLL:       A->spinTarget = (GetRandomValue(0, 1) ? 3 : 4); A->spinAccum = 0.0f; A->speed = 1.7f; break;
    case SPIN_ROLL_UNDO:  A->speed = 1.7f; break;
    case TURN_DIVE:       A->turnYawDelta = Frand(-60.0f, +60.0f); A->speed = 1.8f; break;
    case SWIM_DIVE:       A->speed = 1.6f; break;
    case TURN_ASCEND:     A->speed = 1.6f; break;
    case SWIM_ASCEND:     A->speed = 1.9f; break;
    case BREACH: {
        A->verticalBoost = 7.0f;             // initial upward velocity (units/s) — tune
        A->breachApex = 0;
        A->descendTimer = 0.0f;
        A->breachEulerStart = (Vector3){ A->yaw, A->pitch, A->roll };
        // gentle random twist for the fall
        A->breachEulerTarget = (Vector3){
            A->yaw + Frand(-18.0f, +22.0f),   // yaw drift
            A->pitch + Frand(+24.0f,  +78.0f),   // nose tilts up a bit
            A->roll + Frand(-122.0f, +122.0f)    // bank either way
        };
        A->speed = 2.2f;
        break;
    }
    case BREACH_END:      A->speed = 1.0f; break;
    case GENTLE_BREACH_SWIM: A->speed = 1.4f; break;
    case GENTLE_BREACH:   A->speed = 1.5f; break;
    }
}

// ------------ One function to tick the FSM ------------
static void FSM_Tick(Whale* A, float t, float dt) {
    PoseResetToBind(&A->model, &A->proc);

    switch (A->state) {

        // 0) Plan → immediately Cruise with a fresh goal
    case SWIM_PLAN: {
        A->goal = PickGoalAroundHome(A);
        EnterState(A, SWIM_CRUISE);
    } break;

                  // 1) Cruise toward goal with your subtle fins/tail; slow near goal; then branch
    case SWIM_CRUISE: {
        float dist = Dist3(A->pos, A->goal);
        float slowZone = A->arriveRadius * 2.0f;
        A->speed = Lerp(A->speed, (dist < slowZone ? 0.9f : 1.5f), 0.05f);
        MoveToward(A, A->goal, dt);
        FacePoint(A, A->goal, dt, 0.06f, 0.05f);

        ApplyCruiseBones(A, t);

        if (Near(A->pos, A->goal, A->arriveRadius)) {
            DecideAfterCruise(A);
        }
    } break;

                    // 2) Underwater barrel rolls, then re-plan
    case SPIN_ROLL: {
        float rollRate = 13.0f * dt;              // was 240, a little slower reads better
        A->roll += rollRate;
        A->spinAccum += rollRate;
        ApplyDiveBones(A, t, 8.0f);
        // NEW: require at least 0.8s AND the target spins.
        if (A->stateTime > 5.8f && A->spinAccum >= 360.0f * A->spinTarget) {
            EnterState(A, SPIN_ROLL_UNDO);
        }
    } break;

    case SPIN_ROLL_UNDO: {
        float rollRate = 13.0f * dt;
        A->roll -= rollRate;
        ApplyDiveBones(A, t, 8.0f);
        if (A->stateTime > 3.3f) { EnterState(A, SWIM_PLAN); }
    } break;
                       // 3) Hard-breach chain: TURN_DIVE → SWIM_DIVE → TURN_ASCEND → SPIN_ROLL → SWIM_ASCEND → BREACH → BREACH_END → PLAN
    case TURN_DIVE: {
        // quick “commit” then start going down
        ApplyDiveBones(A, t, 18.0f);
        // small turning nudge (we’re doing position-only, so just time-gate)
        if (A->stateTime > 0.9f) EnterState(A, SWIM_DIVE);
    } break;

    case SWIM_DIVE: {
        // head toward same X/Z as goal, but descend to bottomY
        Vector3 tgt = (Vector3){ A->pos.x, A->bottomY, A->pos.z };
        A->speed = 1.8f;
        MoveToward(A, tgt, dt);
        FacePoint(A, tgt, dt, 0.06f, 0.05f);
        ApplyDiveBones(A, t, 14.0f);
        if (A->pos.y <= A->bottomY + 0.25f) EnterState(A, TURN_ASCEND);
    } break;

    case TURN_ASCEND: {
        // brief settle, then do a quick spin before swimming up fast
        ApplyDiveBones(A, t, 10.0f);
        if (A->stateTime > 0.4f) EnterState(A, SWIM_ASCEND);
    } break;

    case SWIM_ASCEND: {
        // ascend faster and faster toward surface, then breach
        Vector3 tgt = (Vector3){ A->pos.x, A->surfaceY + 1.0f, A->pos.z };
        A->speed = Lerp(A->speed, 6.4f, 0.03f);
        MoveToward(A, tgt, dt);
        FacePoint(A, tgt, dt, 0.06f, 0.05f);
        ApplyDiveBones(A, t, 16.0f);
        if (A->pos.y >= A->surfaceY) EnterState(A, BREACH);
    } break;

    case BREACH: {
        // 1) Tail punch (just to sell the launch visually)
        ApplyBreachBones(A, t, 22.0f);

        // 2) Simple ballistic vertical motion
        const float g = 12.0f;                  // gravity-ish (units/s^2) — tune
        A->pos.y += A->verticalBoost * dt;
        A->verticalBoost -= g * dt;

        // 3) Small forward drift (use yaw heading)
        Vector3 fwd = (Vector3){ sinf(DEG2RAD * A->yaw), 0.0f, cosf(DEG2RAD * A->yaw) };
        A->pos = Vector3Add(A->pos, Vector3Scale(fwd, A->speed * dt));

        // 4) Start the gentle twist only AFTER the apex
        // Roll starts immediately (stateTime-based). Yaw/Pitch wait for apex (descendTimer-based).
        // Roll: ease from entry to target over ~1.5s from the very start of BREACH.
        float kRoll = SmoothStep(Clamp01(A->stateTime / 1.5f));
        A->roll = AngleLerpDeg(A->breachEulerStart.z, A->breachEulerTarget.z, kRoll);
        // Trigger apex if we haven't yet
        if (!A->breachApex && A->verticalBoost <= 0.0f) {
            A->breachApex = 1;
            A->descendTimer = 0.0f;
        }
        // Yaw/Pitch: only start blending after the apex, over ~1.2s.
        if (A->breachApex) {
            A->descendTimer += dt;
            float kFall = SmoothStep(Clamp01(A->descendTimer / 1.2f));
            A->yaw = AngleLerpDeg(A->breachEulerStart.x, A->breachEulerTarget.x, kFall);
            A->pitch = AngleLerpDeg(A->breachEulerStart.y, A->breachEulerTarget.y, kFall);
        }

        // 5) Exit: once we’ve fallen back under the surface (or time safety)
        if (A->pos.y <= A->surfaceY - 1.11f || A->stateTime > 6.0f) {
            EnterState(A, BREACH_END);
        }
    } break;

    case BREACH_END: {
        // gently correct, then re-plan
        A->roll = AngleLerpDeg(A->roll, 0.0f, 0.08f);
        A->pitch = AngleLerpDeg(A->pitch, 0.0f, 0.08f);
        A->yaw = AngleLerpDeg(A->yaw, 0.0f, 0.08f);
        if (A->stateTime > 2.6f) EnterState(A, SWIM_PLAN);
    } break;

                   // 4) Gentle-breach chain: SWIM near-surface → arc → (BLOW | TAIL_SLAP) → PLAN
    case GENTLE_BREACH_SWIM: {
        // set a near-surface waypoint above current XZ, then swim to it
        Vector3 tgt = (Vector3){ A->pos.x, A->surfaceY - 0.8f, A->pos.z };
        MoveToward(A, tgt, dt);
        FacePoint(A, tgt, dt, 0.06f, 0.05f);
        ApplyCruiseBones(A, t);
        if (Near(A->pos, tgt, 0.2f)) { EnterState(A, GENTLE_BREACH); }
    } break;

    case GENTLE_BREACH: {
        // Arc the back slowly (visually expose the back)
        float s = A->stateTime / 1.2f;
        float rate = 16.0f * dt;
        A->pitch += rate;
        SetFromBindPlusEuler(&A->model, &A->proc, WHALE_BONE_AB, DEG2RAD * (-6.0f * s), 0, 0);
        SetFromBindPlusEuler(&A->model, &A->proc, WHALE_BONE_CHEST, DEG2RAD * (-4.0f * s), 0, 0);
        SetFromBindPlusEuler(&A->model, &A->proc, WHALE_BONE_TAIL, DEG2RAD * (-8.0f * s), 0, 0);

        // NEW: slow forward glide along heading (+Z in model space is forward)
        const float glideSpeed = 0.58f;                         // tune: 0.6–1.2 feels right
        Vector3 fwd = (Vector3){ sinf(DEG2RAD * A->yaw), 0.0f, cosf(DEG2RAD * A->yaw) };
        A->pos = Vector3Add(A->pos, Vector3Scale(fwd, glideSpeed * dt));

        // NEW: gently keep the body near the surface while gliding
        A->pos.y = Lerp(A->pos.y, A->surfaceY - 0.6f, 0.04f); // lower = more “skimming”

        if (A->stateTime > 6.2f) { EnterState(A, SWIM_PLAN); }
    } break;
    }

    UpdateModelAnimation(A->model, A->proc, 0);
    A->stateTime += dt;
}

#endif // WHALE_H