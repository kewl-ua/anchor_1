#include "render/soldiers.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <vector>

#include "render/iso.h"

namespace render::soldiers {
namespace {

// A tile on the ground in pixels, as the figure is drawn (across the screen,
// before the ground is squashed to half its height).
constexpr float kPxPerTile = iso::kTileWidth * 0.5f * 1.41421356f;

constexpr std::array<int, static_cast<size_t>(Pose::Count)> kFrames = {2, 8, 2, 2, 2, 2, 3, 1, 4, 2, 1, 2, 1, 2, 8, 8, 2};

// A point on him: ahead, to his left, up (pixels).
struct V3 {
    float f = 0.0f;
    float l = 0.0f;
    float z = 0.0f;
};
V3 operator+(V3 a, V3 b) { return {a.f + b.f, a.l + b.l, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.f - b.f, a.l - b.l, a.z - b.z}; }
V3 operator*(V3 a, float k) { return {a.f * k, a.l * k, a.z * k}; }
V3 mix(V3 a, V3 b, float t) { return a + (b - a) * t; }
float dot(V3 a, V3 b) { return a.f * b.f + a.l * b.l + a.z * b.z; }
float length(V3 a) { return std::sqrt(dot(a, a)); }
V3 unit(V3 a) {
    const float n = length(a);
    return n > 0.0f ? a * (1.0f / n) : V3{1.0f, 0.0f, 0.0f};
}

// Where the knee, the elbow comes: bones `bone` long from `a` to `b`, bent the way of `hint`.
V3 joint(V3 a, V3 b, float bone, V3 hint) {
    const V3 d = b - a;
    const float dist = length(d);
    const V3 mid = mix(a, b, 0.5f);
    if (dist >= bone * 2.0f - 0.01f) return mid;
    const V3 du = unit(d);
    V3 across = hint - du * dot(hint, du);
    if (length(across) < 0.001f) across = {0.0f, 0.0f, -1.0f};
    return mid + unit(across) * std::sqrt(bone * bone - dist * dist * 0.25f);
}

// His figure in a pose: [0] his right, [1] his left.
struct Body {
    V3 pelvis, chest, head;
    V3 shoulder[2], hand[2];
    V3 hip[2], knee[2], foot[2];
    // His weapon from butt to muzzle, or `slung` on his back; `none`: neither
    // (a crew at the weapon set up in front of it, drawn apart).
    V3 butt, muzzle;
    bool slung = false;
    bool none = false;
    // A tool in his hands: an axe (a rear trooper's), a spade, from his hands to its head; earth on the spade.
    bool tool = false;
    V3 tool_a, tool_b;
    bool axe = false;
    bool pick = false;  // the tool's a pickaxe
    bool dirt = false;
    bool log = false;   // a log on his right shoulder, along the way he faces
    bool sack = false;  // a sack of stone on his back
    bool grenade = false;  // one in his right hand
    bool bipod = false;    // the machine gun on its legs
};

Body mix(const Body& a, const Body& b, float t) {
    Body m = t < 0.5f ? a : b;
    m.pelvis = mix(a.pelvis, b.pelvis, t);
    m.chest = mix(a.chest, b.chest, t);
    m.head = mix(a.head, b.head, t);
    for (int s = 0; s < 2; ++s) {
        m.shoulder[s] = mix(a.shoulder[s], b.shoulder[s], t);
        m.hand[s] = mix(a.hand[s], b.hand[s], t);
        m.hip[s] = mix(a.hip[s], b.hip[s], t);
        m.knee[s] = mix(a.knee[s], b.knee[s], t);
        m.foot[s] = mix(a.foot[s], b.foot[s], t);
    }
    m.butt = mix(a.butt, b.butt, t);
    m.muzzle = mix(a.muzzle, b.muzzle, t);
    return m;
}

// His weapon: how long, where his hands go on it (along it, as fractions).
struct Arm {
    float length;
    float grip;
    float fore;
};
Arm arm_of(Kit k) {
    switch (k) {
        case Kit::Rifle: return {9.0f, 0.34f, 0.62f};
        case Kit::MachineGun: return {11.0f, 0.3f, 0.5f};
        case Kit::Rpg: return {12.5f, 0.42f, 0.56f};
        case Kit::Sniper: return {12.0f, 0.3f, 0.55f};
        case Kit::Assault: return {8.5f, 0.34f, 0.62f};
        case Kit::Sapper:
        case Kit::Radio: return {7.0f, 0.38f, 0.66f};
        case Kit::Igla: return {13.0f, 0.42f, 0.56f};
        case Kit::Mortar: return {10.0f, 0.4f, 0.6f};
        case Kit::Ags: return {7.0f, 0.4f, 0.6f};
        default: return {0.0f, 0.0f, 0.0f};
    }
}
bool on_shoulder(Kit k) { return k == Kit::Rpg || k == Kit::Igla; }
// Carried on his back but when he fights with it.
bool carried(Kit k) { return k == Kit::Rpg || k == Kit::Igla || k == Kit::Mortar || k == Kit::Ags; }
bool unarmed(Kit k) { return k == Kit::Rear; }

void upper(Body& b, V3 chest, V3 head) {
    b.chest = chest;
    b.head = head;
    b.shoulder[0] = chest + V3{-0.1f, -2.25f, -0.25f};
    b.shoulder[1] = chest + V3{-0.1f, 2.25f, -0.25f};
}
void hips(Body& b) {
    b.hip[0] = b.pelvis + V3{0.0f, -1.2f, -0.3f};
    b.hip[1] = b.pelvis + V3{0.0f, 1.2f, -0.3f};
}
void stand_legs(Body& b, float bend = 0.0f) {
    b.pelvis = {0.0f, 0.0f, 9.6f - bend};
    hips(b);
    for (int s = 0; s < 2; ++s) {
        const float side = s == 0 ? -1.0f : 1.0f;
        b.foot[s] = {0.0f, side * 1.45f, 0.3f};
        b.knee[s] = {0.35f + bend * 0.7f, side * 1.35f, 4.9f - bend * 0.5f};
    }
}
void stance_legs(Body& b) {  // feet apart, the right one back
    b.pelvis = {-0.1f, 0.0f, 9.2f};
    hips(b);
    b.foot[0] = {-1.9f, -1.3f, 0.3f};
    b.foot[1] = {1.9f, 1.35f, 0.3f};
    b.knee[0] = {-0.8f, -1.3f, 4.9f};
    b.knee[1] = {1.6f, 1.35f, 4.8f};
}
void kneel_legs(Body& b) {  // down on his right knee
    b.pelvis = {-0.6f, 0.0f, 5.3f};
    hips(b);
    b.knee[0] = {0.8f, -1.3f, 0.6f};
    b.foot[0] = {-3.6f, -1.3f, 0.5f};
    b.knee[1] = {2.9f, 1.4f, 4.6f};
    b.foot[1] = {3.0f, 1.4f, 0.3f};
}
void hands_hanging(Body& b, float swing = 0.0f) {
    b.hand[0] = {b.pelvis.f + 0.4f - swing, -2.6f, b.pelvis.z - 0.2f};
    b.hand[1] = {b.pelvis.f + 0.4f + swing, 2.6f, b.pelvis.z - 0.2f};
}

// His weapon in his hands, from its butt along `dir`.
void hold(Body& b, Kit k, V3 butt, V3 dir) {
    const Arm a = arm_of(k);
    dir = unit(dir);
    b.butt = butt;
    b.muzzle = butt + dir * a.length;
    b.hand[0] = butt + dir * (a.length * a.grip) + V3{0.0f, 0.0f, -0.6f};
    b.hand[1] = butt + dir * (a.length * a.fore) + V3{0.0f, 0.0f, -0.4f};
}
// Held low across him, the muzzle down ahead: at the ready.
void low_ready(Body& b, Kit k) { hold(b, k, b.chest + V3{0.4f, -1.7f, -2.3f}, {1.0f, 0.3f, -0.45f}); }
// Shouldered, aimed ahead; `kick` pushes it back and up.
void shouldered(Body& b, Kit k, float kick) {
    if (on_shoulder(k)) {  // a tube over his right shoulder, his hands on its grips under it
        const Arm a = arm_of(k);
        const V3 dir = unit({1.0f, 0.06f, kick * 0.1f});
        const V3 middle = b.shoulder[0] + V3{0.3f - kick, 0.2f, 1.2f};
        b.butt = middle - dir * (a.length * 0.45f);
        b.muzzle = middle + dir * (a.length * 0.55f);
        b.hand[0] = middle + dir * 1.6f + V3{0.0f, 0.3f, -1.5f};
        b.hand[1] = middle + dir * 3.8f + V3{0.0f, 0.6f, -1.3f};
        return;
    }
    hold(b, k, b.shoulder[0] + V3{0.35f - kick, 0.45f, -0.25f}, {1.0f, 0.1f, 0.02f + kick * 0.2f});
}

Body pose(Kit kit, Pose p, int i) {
    Body b;
    const bool free_hands = carried(kit) || unarmed(kit);
    switch (p) {
        case Pose::Stand: {
            stand_legs(b);
            const float breath = static_cast<float>(i) * 0.3f;
            upper(b, {0.1f, 0.0f, 14.4f + breath}, {0.35f, 0.0f, 17.6f + breath});
            if (unarmed(kit)) {  // a spade over his shoulder
                hands_hanging(b);
                b.hand[0] = {1.0f, -2.3f, 13.2f + breath};
                b.tool = true;
                b.tool_a = b.hand[0] + V3{1.0f, 0.0f, -0.4f};
                b.tool_b = {-3.8f, -2.2f, 17.4f + breath};
            } else if (carried(kit)) {
                b.slung = true;
                hands_hanging(b);
            } else {
                low_ready(b, kit);
            }
            break;
        }
        case Pose::CarryLog:
        case Pose::CarrySack: {
            // The walk under a load: shorter steps, leaning into it a little.
            const float t = static_cast<float>(i) / 8.0f * 6.2831853f;
            const float sw = std::sin(t);
            const float cw = std::cos(t);
            const float bob = 0.4f * sw * sw;
            b.pelvis = {0.15f, 0.0f, 9.5f - bob};
            hips(b);
            b.foot[0] = {2.0f * sw, -1.3f, 0.3f + std::max(0.0f, cw) * 1.1f};
            b.foot[1] = {-2.0f * sw, 1.3f, 0.3f + std::max(0.0f, -cw) * 1.1f};
            for (int k = 0; k < 2; ++k) {
                const float lift = b.foot[k].z - 0.3f;
                b.knee[k] = mix(b.hip[k], b.foot[k], 0.5f) + V3{0.9f + lift * 0.7f, 0.0f, lift * 0.2f};
            }
            b.slung = !unarmed(kit);
            if (p == Pose::CarryLog) {  // the log on the right shoulder, the right hand on it in front, the left swinging
                upper(b, {0.7f, 0.15f, 14.2f - bob}, {1.0f, 0.5f, 17.2f - bob});
                b.log = true;
                b.hand[0] = b.shoulder[0] + V3{1.8f, 0.3f, 1.2f};
                b.hand[1] = {b.pelvis.f + 0.4f - 1.4f * sw, 2.6f, b.pelvis.z - 0.2f};
            } else {  // the sack over the right shoulder, held by its neck, bent under it
                upper(b, {0.9f, 0.0f, 13.9f - bob}, {1.4f, -0.2f, 16.8f - bob});
                b.sack = true;
                b.hand[0] = b.shoulder[0] + V3{0.3f, 0.2f, 1.6f};
                b.hand[1] = {b.pelvis.f + 0.4f - 1.4f * sw, 2.6f, b.pelvis.z - 0.2f};
            }
            break;
        }
        case Pose::Heave: {  // up with it, out and away (the load itself flies apart, drawn by the game)
            stance_legs(b);
            if (i == 0) {
                upper(b, {0.2f, 0.0f, 13.6f}, {0.5f, 0.0f, 16.6f});
                b.hand[0] = {2.2f, -1.4f, 13.4f};
                b.hand[1] = {2.2f, 1.4f, 13.4f};
            } else {
                upper(b, {1.2f, 0.0f, 14.2f}, {1.8f, 0.0f, 17.2f});
                b.hand[0] = {4.4f, -1.3f, 17.8f};
                b.hand[1] = {4.4f, 1.3f, 17.8f};
            }
            b.none = true;
            break;
        }
        case Pose::Quarry: {  // a pickaxe: up over his head, down into the rock ahead
            b.pelvis = {0.0f, 0.0f, 9.2f};
            hips(b);
            b.foot[0] = {-1.3f, -1.5f, 0.3f};
            b.foot[1] = {1.7f, 1.5f, 0.3f};
            b.knee[0] = {-0.3f, -1.45f, 4.8f};
            b.knee[1] = {1.9f, 1.45f, 4.7f};
            b.tool = true;
            b.pick = true;
            b.slung = !unarmed(kit);
            if (i == 0) {
                upper(b, {-0.3f, 0.0f, 14.4f}, {0.1f, 0.0f, 17.5f});
                b.hand[0] = {-0.6f, -0.6f, 19.2f};
                b.hand[1] = {-0.2f, 0.4f, 18.0f};
                b.tool_b = {-3.6f, 0.0f, 20.4f};
            } else {
                upper(b, {2.0f, 0.0f, 12.2f}, {2.9f, 0.0f, 14.8f});
                b.hand[0] = {3.8f, -0.4f, 9.2f};
                b.hand[1] = {3.0f, 0.4f, 10.4f};
                b.tool_b = {7.6f, 0.0f, 2.6f};
            }
            b.tool_a = mix(b.hand[0], b.hand[1], 0.5f);
            break;
        }
        case Pose::Walk: {
            const float t = static_cast<float>(i) / static_cast<float>(kFrames[static_cast<size_t>(Pose::Walk)]) * 6.2831853f;
            const float s = std::sin(t);
            const float c = std::cos(t);
            const float bob = 0.35f * s * s;
            b.pelvis = {0.2f, 0.0f, 9.6f - bob};
            hips(b);
            b.foot[0] = {2.5f * s, -1.3f, 0.3f + std::max(0.0f, c) * 1.3f};
            b.foot[1] = {-2.5f * s, 1.3f, 0.3f + std::max(0.0f, -c) * 1.3f};
            for (int k = 0; k < 2; ++k) {
                const float lift = b.foot[k].z - 0.3f;
                b.knee[k] = mix(b.hip[k], b.foot[k], 0.5f) + V3{0.9f + lift * 0.7f, 0.0f, lift * 0.2f};
            }
            upper(b, {0.55f, 0.0f, 14.4f - bob}, {0.9f, 0.0f, 17.5f - bob});
            if (free_hands) {
                b.slung = carried(kit);
                hands_hanging(b, 1.7f * s);
                if (unarmed(kit)) {
                    b.hand[0] = {1.3f, -2.3f, 13.2f - bob};
                    b.tool = true;
                    b.tool_a = b.hand[0] + V3{1.0f, 0.0f, -0.4f};
                    b.tool_b = {-3.4f, -2.2f, 17.4f - bob};
                }
            } else {
                low_ready(b, kit);
            }
            break;
        }
        case Pose::Aim:
        case Pose::Kneel: {
            const float kick = static_cast<float>(i) * 0.7f;
            if (p == Pose::Aim) {
                stance_legs(b);
                upper(b, {0.7f - kick * 0.5f, -0.2f, 14.0f}, {1.25f - kick * 0.5f, -0.45f, 16.9f});
            } else {
                kneel_legs(b);
                upper(b, {0.5f - kick * 0.5f, -0.2f, 10.2f}, {1.1f - kick * 0.5f, -0.45f, 13.1f});
            }
            if (free_hands && !on_shoulder(kit)) {
                b.none = !unarmed(kit);
                hands_hanging(b);
            } else {
                shouldered(b, kit, kick);
            }
            break;
        }
        case Pose::Prone: {
            const float kick = static_cast<float>(i) * 0.5f;
            b.pelvis = {-4.5f, 0.0f, 1.5f};
            hips(b);
            b.knee[0] = {-8.8f, -1.8f, 0.9f};
            b.knee[1] = {-8.8f, 1.6f, 0.9f};
            b.foot[0] = {-12.6f, -2.6f, 0.8f};
            b.foot[1] = {-12.4f, 2.1f, 0.8f};
            upper(b, {1.0f - kick, 0.0f, 2.3f}, {3.5f - kick, -0.4f, 3.9f});
            b.shoulder[0].z = b.shoulder[1].z = 2.6f;
            hold(b, kit, {1.6f - kick, -1.1f, 3.7f}, {1.0f, 0.05f, 0.02f + kick * 0.15f});
            b.bipod = kit == Kit::MachineGun;
            break;
        }
        case Pose::Reload: {
            kneel_legs(b);
            upper(b, {0.6f, -0.1f, 10.2f}, {1.1f, -0.2f, 13.0f});
            if (free_hands && !on_shoulder(kit)) {
                b.none = !unarmed(kit);
                hands_hanging(b);
                break;
            }
            const V3 dir = unit({0.4f, 0.12f, 1.0f});
            hold(b, kit, {1.6f, -1.0f, 6.6f}, dir);
            const float len = arm_of(kit).length;
            if (on_shoulder(kit)) {  // a rocket into the tube's mouth
                b.hand[1] = i == 0 ? b.muzzle + V3{0.3f, 0.6f, 0.6f} : b.muzzle + V3{0.2f, 0.5f, -0.8f};
            } else {  // a fresh magazine
                b.hand[1] = i == 0 ? b.butt + dir * (len * 0.45f) + V3{0.9f, 0.6f, -0.6f} : b.butt + dir * (len * 0.3f) + V3{1.3f, 0.9f, -1.8f};
            }
            break;
        }
        case Pose::Throw: {
            stance_legs(b);
            b.slung = !unarmed(kit);
            b.grenade = i < 2;
            if (i == 0) {  // wound back, his other arm out at the target
                upper(b, {-1.0f, 0.0f, 13.9f}, {-0.8f, 0.0f, 17.0f});
                b.hand[0] = {-5.2f, -2.9f, 17.2f};
                b.hand[1] = {3.6f, 2.2f, 14.6f};
            } else if (i == 1) {  // over the top
                upper(b, {0.6f, 0.0f, 14.2f}, {1.0f, 0.0f, 17.3f});
                b.hand[0] = {1.4f, -2.4f, 21.8f};
                b.hand[1] = {1.0f, 2.6f, 10.8f};
            } else {  // through, bent after it
                upper(b, {2.0f, 0.0f, 13.0f}, {2.8f, 0.0f, 15.6f});
                b.hand[0] = {6.4f, -1.2f, 11.6f};
                b.hand[1] = {-1.4f, 2.5f, 10.0f};
            }
            break;
        }
        case Pose::Hurt: {
            stand_legs(b, 0.8f);
            upper(b, {1.1f, 0.0f, 13.4f}, {1.8f, 0.2f, 16.0f});
            if (free_hands) {
                b.slung = carried(kit);
                b.hand[0] = {1.6f, -1.5f, 11.0f};
            } else {  // his weapon hanging from his right hand
                const V3 dir = unit({0.5f, 0.15f, -0.85f});
                const Arm a = arm_of(kit);
                const V3 grip{1.3f, -2.2f, 10.6f};
                b.butt = grip - dir * (a.length * a.grip);
                b.muzzle = b.butt + dir * a.length;
                b.hand[0] = grip;
            }
            b.hand[1] = {1.9f, 0.6f, 11.3f};  // clutching himself
            break;
        }
        case Pose::Die: {
            const Arm a = arm_of(kit);
            Body reel;  // hit: reeling back, his arms flung out, his weapon falling
            stand_legs(reel, 1.2f);
            upper(reel, {-1.3f, 0.0f, 13.3f}, {-2.1f, 0.0f, 15.8f});
            reel.hand[0] = {-0.6f, -3.9f, 16.4f};
            reel.hand[1] = {-1.2f, 3.7f, 15.6f};
            reel.butt = {1.4f, -2.6f, 8.4f};
            reel.muzzle = reel.butt + unit({5.5f, -1.0f, -3.5f}) * a.length;
            Body knees;  // down on his knees, slumping back; the weapon on the ground
            knees.pelvis = {-0.4f, 0.0f, 4.4f};
            hips(knees);
            knees.knee[0] = {1.4f, -1.3f, 0.6f};
            knees.knee[1] = {1.4f, 1.3f, 0.6f};
            knees.foot[0] = {-2.6f, -1.4f, 0.5f};
            knees.foot[1] = {-2.6f, 1.4f, 0.5f};
            upper(knees, {-2.1f, 0.0f, 8.8f}, {-3.1f, 0.0f, 11.1f});
            knees.hand[0] = {0.4f, -2.9f, 5.0f};
            knees.hand[1] = {0.2f, 2.9f, 5.2f};
            knees.butt = {2.2f, -3.4f, 0.4f};
            knees.muzzle = knees.butt + unit({1.0f, -0.15f, 0.0f}) * a.length;
            Body lying = knees;  // on his back, arms out
            lying.pelvis = {-1.5f, 0.0f, 1.0f};
            hips(lying);
            lying.knee[0] = {2.6f, -1.7f, 0.9f};
            lying.knee[1] = {2.5f, 1.9f, 1.2f};
            lying.foot[0] = {6.6f, -2.4f, 0.5f};
            lying.foot[1] = {6.3f, 2.7f, 0.5f};
            upper(lying, {-6.6f, 0.0f, 1.0f}, {-9.2f, 0.3f, 1.1f});
            lying.hand[0] = {-5.0f, -5.8f, 0.5f};
            lying.hand[1] = {-7.4f, 4.9f, 0.5f};
            for (Body* k : {&reel, &knees, &lying}) {
                k->slung = carried(kit);
                k->none = unarmed(kit);
            }
            b = i == 0 ? reel : i == 1 ? knees : i == 2 ? mix(knees, lying, 0.55f) : lying;
            break;
        }
        case Pose::Work: {
            b.pelvis = {0.0f, 0.0f, 9.2f};
            hips(b);
            b.foot[0] = {-1.3f, -1.5f, 0.3f};
            b.foot[1] = {1.7f, 1.5f, 0.3f};
            b.knee[0] = {-0.3f, -1.45f, 4.8f};
            b.knee[1] = {1.9f, 1.45f, 4.7f};
            b.slung = !unarmed(kit);
            b.tool = true;
            b.axe = unarmed(kit);
            if (unarmed(kit)) {  // an axe, up and down
                if (i == 0) {
                    upper(b, {-0.4f, 0.0f, 14.4f}, {0.0f, 0.0f, 17.5f});
                    b.hand[0] = {-0.4f, -0.7f, 18.9f};
                    b.hand[1] = {-0.1f, 0.3f, 17.9f};
                    b.tool_b = {-3.1f, 0.0f, 21.2f};
                } else {
                    upper(b, {1.9f, 0.0f, 12.5f}, {2.7f, 0.0f, 15.1f});
                    b.hand[0] = {4.0f, -0.4f, 9.6f};
                    b.hand[1] = {3.3f, 0.4f, 10.6f};
                    b.tool_b = {8.0f, 0.0f, 5.0f};
                }
                b.tool_a = mix(b.hand[0], b.hand[1], 0.5f);
            } else {  // a spade, in and up with the earth
                if (i == 0) {
                    upper(b, {1.2f, 0.0f, 13.2f}, {1.9f, 0.0f, 16.1f});
                    b.hand[0] = {2.4f, -0.6f, 10.8f};
                    b.hand[1] = {3.3f, 0.3f, 8.2f};
                    b.tool_a = {2.1f, -0.7f, 12.2f};
                    b.tool_b = {5.0f, 0.3f, 0.3f};
                } else {
                    upper(b, {1.6f, 0.0f, 12.8f}, {2.4f, 0.0f, 15.6f});
                    b.hand[0] = {3.2f, -0.5f, 11.8f};
                    b.hand[1] = {4.4f, 0.3f, 9.4f};
                    b.tool_a = {2.8f, -0.6f, 13.0f};
                    b.tool_b = {7.4f, 0.3f, 4.6f};
                    b.dirt = true;
                }
            }
            break;
        }
        case Pose::Sit: {
            b.pelvis = {-1.2f, 0.0f, 1.8f};
            hips(b);
            b.knee[0] = {2.4f, -1.6f, 4.2f};
            b.knee[1] = {2.4f, 1.6f, 4.2f};
            b.foot[0] = {4.6f, -1.7f, 0.4f};
            b.foot[1] = {4.6f, 1.7f, 0.4f};
            upper(b, {-1.8f, 0.0f, 7.3f}, {-1.3f, -0.2f, 10.1f});
            b.hand[0] = {2.0f, -1.9f, 4.6f};
            b.hand[1] = {2.0f, 1.9f, 4.6f};
            if (unarmed(kit)) {
                b.none = true;
            } else if (carried(kit)) {  // laid down beside him
                const float len = arm_of(kit).length;
                b.butt = {-2.5f, -4.4f, 0.4f};
                b.muzzle = b.butt + V3{len, 0.4f, 0.0f};
            } else {  // across his knees
                hold(b, kit, {0.2f, -3.1f, 3.6f}, {6.3f, 5.6f, 1.4f});
            }
            break;
        }
        case Pose::Climb: {
            b.pelvis = {0.0f, 0.0f, 9.6f};
            hips(b);
            const bool right_up = i == 0;
            b.foot[0] = {1.0f, -1.3f, right_up ? 2.8f : 0.3f};
            b.knee[0] = {right_up ? 2.4f : 1.3f, -1.3f, right_up ? 6.6f : 4.9f};
            b.foot[1] = {1.0f, 1.3f, right_up ? 0.3f : 2.8f};
            b.knee[1] = {right_up ? 1.3f : 2.4f, 1.3f, right_up ? 4.9f : 6.6f};
            upper(b, {0.5f, 0.0f, 14.4f}, {0.9f, 0.0f, 17.6f});
            b.hand[0] = {1.5f, -1.7f, right_up ? 19.2f : 16.2f};
            b.hand[1] = {1.5f, 1.7f, right_up ? 16.2f : 19.2f};
            b.slung = !unarmed(kit);
            b.none = unarmed(kit);
            break;
        }
        case Pose::Crew:
        default: {
            kneel_legs(b);
            upper(b, {1.2f, 0.0f, 9.9f}, {1.9f, 0.0f, 12.7f});
            b.hand[0] = {3.7f, -1.1f, 7.0f};
            b.hand[1] = {3.9f, 1.0f, 7.4f};
            b.none = true;
            break;
        }
    }
    if (unarmed(kit) && !b.tool) b.none = true;
    return b;
}

// --- Drawing him into pixels -------------------------------------------------

Color shade(Color c, float k) {
    auto ch = [k](unsigned char v) { return static_cast<unsigned char>(std::clamp(static_cast<float>(v) * k, 0.0f, 255.0f)); };
    return {ch(c.r), ch(c.g), ch(c.b), c.a};
}
Color mix(Color a, Color b, float t) {
    auto ch = [t](unsigned char x, unsigned char y) {
        return static_cast<unsigned char>(static_cast<float>(x) + (static_cast<float>(y) - static_cast<float>(x)) * t);
    };
    return {ch(a.r, b.r), ch(a.g, b.g), ch(a.b, b.b), 255};
}
float hash01(uint32_t a, int x, int y) {
    uint32_t h = a * 0x9E3779B1u ^ static_cast<uint32_t>(x) * 0x85EBCA77u ^ static_cast<uint32_t>(y) * 0xC2B2AE3Du;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return static_cast<float>(h & 0xFFFFu) / 65536.0f;
}

// A cloth, a leather, a steel: its colour and its camouflage.
struct Paint {
    Color base;
    Color spot_dark{};
    Color spot_light{};
    bool camo = false;
    uint32_t seed = 0;
};

constexpr Color kInk{26, 24, 22, 255};

// A figure drawn into a frame from one direction: every point on him put on
// the screen, his parts painted from the farthest in; a part's edge inked
// where it lies over what's behind it, then the whole of him outlined.
class Figure {
public:
    explicit Figure(float angle) : ca_(std::cos(angle)), sa_(std::sin(angle)) {
        px_.fill({0, 0, 0, 0});
        stamp_.fill(0);
    }
    Vector2 at(V3 p) const { return {kOriginX + p.f * ca_ + p.l * sa_, kOriginY + 0.5f * (p.f * sa_ - p.l * ca_) - p.z}; }
    float depth(V3 p) const { return p.f * sa_ - p.l * ca_; }  // nearer the viewer: more

    void begin(float dim = 1.0f, bool inked = true) {
        stage_.clear();
        ++part_;
        dim_ = dim;
        inked_ = inked;
    }
    void end() {
        for (const Stage& s : stage_) stamp_[static_cast<size_t>(s.i)] = part_;
        for (const Stage& s : stage_) {
            Color c = s.c;
            if (inked_) {
                const int x = s.i % kCellW;
                const int y = s.i / kCellW;
                for (const auto [dx, dy] : {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}}) {
                    const int nx = x + dx;
                    const int ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= kCellW || ny >= kCellH) continue;
                    const size_t n = static_cast<size_t>(ny * kCellW + nx);
                    if (stamp_[n] != part_ && px_[n].a > 0) {
                        c = s.edge;
                        break;
                    }
                }
            }
            px_[static_cast<size_t>(s.i)] = c;
        }
    }

    // A limb, a body: round, from a to b, its radius ra to rb, lit from the upper left.
    // `from`, `to`: only the stretch of it between those (fractions along it): a band round it.
    void capsule(V3 a3, V3 b3, float ra, float rb, const Paint& p, float from = -9.0f, float to = 9.0f) {
        const Vector2 a = at(a3);
        const Vector2 b = at(b3);
        const Vector2 d{b.x - a.x, b.y - a.y};
        const float dd = d.x * d.x + d.y * d.y;
        const float seg = std::sqrt(dd);
        const float r = std::max(ra, rb);
        for (int y = static_cast<int>(std::floor(std::min(a.y, b.y) - r)); y <= static_cast<int>(std::ceil(std::max(a.y, b.y) + r)); ++y) {
            for (int x = static_cast<int>(std::floor(std::min(a.x, b.x) - r)); x <= static_cast<int>(std::ceil(std::max(a.x, b.x) + r)); ++x) {
                const Vector2 q{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f};
                const float raw = dd > 0.0001f ? ((q.x - a.x) * d.x + (q.y - a.y) * d.y) / dd : 0.0f;
                if (raw < from || raw > to) continue;
                const float t = std::clamp(raw, 0.0f, 1.0f);
                const Vector2 axis_at{a.x + d.x * t, a.y + d.y * t};
                const float rr = ra + (rb - ra) * t;
                const float nx = (q.x - axis_at.x) / std::max(0.5f, rr);
                const float ny = (q.y - axis_at.y) / std::max(0.5f, rr);
                const float out = nx * nx + ny * ny;
                if (out > 1.0f) continue;
                float u = t * seg;
                float v = seg > 0.0001f ? ((q.x - a.x) * d.y - (q.y - a.y) * d.x) / seg : q.y - a.y;
                if (seg <= 0.0001f) u = q.x - a.x;
                put(x, y, tone(p, nx, ny, std::sqrt(1.0f - out), static_cast<int>(std::floor(u / 1.7f)), static_cast<int>(std::floor((v + 20.0f) / 1.7f))),
                    shade(p.base, 0.55f * dim_));
            }
        }
    }
    // A head, a helmet: a ball; `cut` keeps only what's above that far below its middle (a helmet's dome).
    void ball(V3 c3, float r, const Paint& p, float cut = 99.0f) {
        const Vector2 c = at(c3);
        for (int y = static_cast<int>(std::floor(c.y - r)); y <= static_cast<int>(std::ceil(c.y + r)); ++y) {
            for (int x = static_cast<int>(std::floor(c.x - r)); x <= static_cast<int>(std::ceil(c.x + r)); ++x) {
                const float nx = (static_cast<float>(x) + 0.5f - c.x) / r;
                const float ny = (static_cast<float>(y) + 0.5f - c.y) / r;
                const float out = nx * nx + ny * ny;
                if (out > 1.0f || static_cast<float>(y) + 0.5f - c.y > cut) continue;
                put(x, y, tone(p, nx, ny, std::sqrt(1.0f - out), x, y), shade(p.base, 0.55f * dim_));
            }
        }
    }
    // Flat on the ground plane at c3: a hat's brim, `r` across.
    void brim(V3 c3, float r, Color base) {
        const Vector2 c = at(c3);
        for (int y = static_cast<int>(std::floor(c.y - r)); y <= static_cast<int>(std::ceil(c.y + r)); ++y) {
            for (int x = static_cast<int>(std::floor(c.x - r)); x <= static_cast<int>(std::ceil(c.x + r)); ++x) {
                const float nx = (static_cast<float>(x) + 0.5f - c.x) / r;
                const float ny = (static_cast<float>(y) + 0.5f - c.y) / (r * 0.5f);
                if (nx * nx + ny * ny > 1.0f) continue;
                put(x, y, shade(base, (ny < 0.0f ? 1.1f : 0.8f) * dim_), shade(base, 0.55f * dim_));
            }
        }
    }
    // A rod: a barrel, a stock, a handle; `thick` 1 or 2 pixels.
    void rod(V3 a3, V3 b3, Color c, int thick = 1) {
        const Vector2 a = at(a3);
        const Vector2 b = at(b3);
        int x0 = static_cast<int>(std::floor(a.x));
        int y0 = static_cast<int>(std::floor(a.y));
        const int x1 = static_cast<int>(std::floor(b.x));
        const int y1 = static_cast<int>(std::floor(b.y));
        const int dx = std::abs(x1 - x0);
        const int dy = -std::abs(y1 - y0);
        const int sx = x0 < x1 ? 1 : -1;
        const int sy = y0 < y1 ? 1 : -1;
        const bool flat = dx >= -dy;
        int err = dx + dy;
        const Color cc = shade(c, dim_);
        for (;;) {
            put(x0, y0, cc, cc);
            if (thick > 1) {
                if (flat) {
                    put(x0, y0 + 1, shade(cc, 0.8f), shade(cc, 0.8f));
                } else {
                    put(x0 + 1, y0, shade(cc, 0.8f), shade(cc, 0.8f));
                }
            }
            if (x0 == x1 && y0 == y1) break;
            const int e2 = 2 * err;
            if (e2 >= dy) {
                err += dy;
                x0 += sx;
            }
            if (e2 <= dx) {
                err += dx;
                y0 += sy;
            }
        }
    }
    void dot(V3 p3, Color c) {
        const Vector2 p = at(p3);
        put(static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y)), shade(c, dim_), shade(c, dim_));
    }

    // His outline all round, then into the sheet at (ox, oy).
    void finish(Color* out, int stride, int ox, int oy) {
        std::array<Color, kCellW * kCellH> inked = px_;
        for (int y = 0; y < kCellH; ++y) {
            for (int x = 0; x < kCellW; ++x) {
                if (px_[static_cast<size_t>(y * kCellW + x)].a > 0) continue;
                bool edge = false;
                for (const auto [dx, dy] : {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}}) {
                    const int nx = x + dx;
                    const int ny = y + dy;
                    if (nx >= 0 && ny >= 0 && nx < kCellW && ny < kCellH && px_[static_cast<size_t>(ny * kCellW + nx)].a > 0) edge = true;
                }
                if (edge) inked[static_cast<size_t>(y * kCellW + x)] = kInk;
            }
        }
        for (int y = 0; y < kCellH; ++y) {
            for (int x = 0; x < kCellW; ++x) out[static_cast<size_t>((oy + y) * stride + ox + x)] = inked[static_cast<size_t>(y * kCellW + x)];
        }
    }

private:
    struct Stage {
        int i;
        Color c;
        Color edge;
    };
    void put(int x, int y, Color c, Color edge) {
        if (x < 0 || y < 0 || x >= kCellW || y >= kCellH) return;
        stage_.push_back({y * kCellW + x, c, edge});
    }
    Color tone(const Paint& p, float nx, float ny, float nz, int cx, int cy) const {
        Color c = p.base;
        if (p.camo) {
            const float h = hash01(p.seed, cx, cy);
            if (h < 0.28f) c = p.spot_dark;
            else if (h > 0.8f) c = p.spot_light;
        }
        const float light = -0.5f * nx - 0.62f * ny + 0.6f * nz;
        const float k = light > 0.6f ? 1.26f : light < 0.14f ? 0.68f : 1.0f;
        return shade(c, k * dim_);
    }

    float ca_;
    float sa_;
    std::array<Color, kCellW * kCellH> px_;
    std::array<int, kCellW * kCellH> stamp_;
    std::vector<Stage> stage_;
    int part_ = 0;
    float dim_ = 1.0f;
    bool inked_ = true;
};

// His side's colours on him.
struct Look {
    Paint cloth;   // jacket and trousers: a pixel camouflage
    Paint vest;    // the plate carrier
    Paint helmet;  // its cover
    Paint skin;
    Paint boots;
    Paint pack;
    Color team;
};

Look look_of(Kit kit, Color team) {
    Look k;
    const Color olive = mix({98, 102, 62, 255}, team, 0.1f);
    k.cloth = {olive, {64, 72, 44, 255}, {112, 90, 58, 255}, true, 17u};
    const Color vest = kit == Kit::Assault ? Color{56, 60, 44, 255} : mix({84, 80, 56, 255}, team, 0.06f);
    k.vest = {vest, shade(vest, 0.8f), shade(vest, 1.1f), false, 5u};
    const Color helmet = mix({82, 92, 60, 255}, team, 0.1f);
    k.helmet = {kit == Kit::Assault ? Color{60, 66, 50, 255} : helmet, shade(helmet, 0.8f), shade(helmet, 1.15f), true, 9u};
    k.skin = {{206, 160, 124, 255}};
    k.boots = {{44, 38, 32, 255}};
    k.pack = {mix({70, 78, 54, 255}, team, 0.08f), shade({70, 78, 54, 255}, 0.8f), {}, false, 3u};
    k.team = team;
    return k;
}

constexpr Color kSteel{36, 36, 38, 255};
constexpr Color kWood{120, 76, 42, 255};
constexpr Color kPolymer{46, 46, 46, 255};
constexpr Color kDrab{88, 98, 64, 255};

// His weapon from butt to muzzle: its stock, its body, its barrel, what hangs off it.
void draw_weapon(Figure& fig, Kit kit, V3 butt, V3 muzzle, bool bipod) {
    const V3 d = muzzle - butt;
    auto along = [&](float t) { return butt + d * t; };
    const V3 down{0.0f, 0.0f, -1.0f};
    switch (kit) {
        case Kit::Rifle:
        case Kit::Assault:
        case Kit::Sapper:
        case Kit::Radio: {  // an AK: its stock, the receiver, the handguard, the barrel, a curved magazine
            const Color furniture = kit == Kit::Rifle ? kWood : kPolymer;
            fig.rod(along(0.0f), along(0.3f), furniture);
            fig.rod(along(0.3f), along(0.56f), kSteel);
            fig.rod(along(0.56f), along(0.76f), furniture);
            fig.rod(along(0.76f), along(1.0f), kSteel);
            const V3 mag = along(0.46f);
            fig.rod(mag, mag + down * 1.6f + unit(d) * 0.7f, {60, 50, 40, 255});
            break;
        }
        case Kit::MachineGun: {  // a PKM: skeleton stock, a long body, the box under it
            fig.rod(along(0.0f), along(0.24f), kWood);
            fig.rod(along(0.24f), along(0.56f), kSteel, 2);
            fig.rod(along(0.56f), along(1.0f), kSteel);
            fig.rod(along(0.38f) + down * 0.9f, along(0.46f) + down * 2.0f, {74, 86, 56, 255}, 2);
            if (bipod) {
                const V3 leg = along(0.82f);
                fig.rod(leg, V3{leg.f + 0.6f, leg.l - 1.0f, 0.0f}, kSteel);
                fig.rod(leg, V3{leg.f + 0.6f, leg.l + 1.0f, 0.0f}, kSteel);
            }
            break;
        }
        case Kit::Sniper: {  // a Dragunov: the skeleton stock, a long barrel, its scope
            fig.rod(along(0.0f), along(0.28f), kWood);
            fig.rod(along(0.28f), along(0.5f), kSteel);
            fig.rod(along(0.5f), along(0.7f), kWood);
            fig.rod(along(0.7f), along(1.0f), kSteel);
            fig.rod(along(0.33f) + V3{0.0f, 0.0f, 1.0f}, along(0.52f) + V3{0.0f, 0.0f, 1.0f}, {20, 20, 22, 255});
            fig.rod(along(0.4f) + down * 0.8f, along(0.44f) + down * 2.0f, kSteel);
            break;
        }
        case Kit::Rpg: {  // an RPG-7: the tube, its wooden guard, the grip, the warhead on its mouth
            fig.rod(along(0.0f), along(0.78f), kDrab, 2);
            fig.rod(along(0.36f), along(0.52f), kWood, 2);
            fig.rod(along(0.44f), along(0.44f) + down * 1.4f, kPolymer);
            fig.rod(along(0.78f), along(1.0f), {66, 84, 52, 255}, 2);
            fig.dot(along(0.93f) + V3{0.0f, 0.0f, 1.0f}, {66, 84, 52, 255});
            fig.dot(along(0.93f) + down * 1.0f, {66, 84, 52, 255});
            break;
        }
        case Kit::Igla: {  // the launch tube, its grip and battery under it, its sight
            fig.rod(along(0.0f), along(1.0f), {92, 104, 70, 255}, 2);
            fig.rod(along(0.4f) + down * 0.8f, along(0.46f) + down * 2.2f, kPolymer, 2);
            fig.rod(along(0.62f) + V3{0.0f, 0.0f, 1.2f}, along(0.7f) + V3{0.0f, 0.0f, 1.2f}, kPolymer);
            break;
        }
        case Kit::Mortar: {  // the tube, its plate
            fig.rod(along(0.0f), along(1.0f), {70, 78, 60, 255}, 2);
            fig.rod(along(0.0f) + V3{0.0f, 1.2f, 0.0f}, along(0.0f) + V3{0.0f, -1.2f, 0.0f}, {56, 58, 52, 255}, 2);
            break;
        }
        case Kit::Ags: {  // the gun, its drum
            fig.rod(along(0.0f), along(1.0f), {66, 72, 58, 255}, 2);
            fig.rod(along(0.4f) + down * 0.6f, along(0.6f) + down * 0.6f, {58, 66, 48, 255}, 2);
            break;
        }
        default: break;
    }
}

// The whole of him from one direction.
void draw_body(Figure& fig, Kit kit, const Body& b, const Look& look) {
    struct Part {
        float depth;
        std::function<void()> draw;
    };
    std::vector<Part> parts;
    const float middle = fig.depth(mix(b.pelvis, b.chest, 0.5f));
    auto dim = [&](float depth) { return depth < middle - 0.4f ? 0.84f : 1.0f; };  // his far side, in shadow

    // Legs: the thigh, the shin, the boot; his side's tape round the thigh.
    for (int s = 0; s < 2; ++s) {
        const float dp = fig.depth(b.knee[s]);
        parts.push_back({dp, [&, s, dp] {
                             fig.begin(dim(dp));
                             fig.capsule(b.hip[s], b.knee[s], 1.35f, 1.15f, look.cloth);
                             fig.capsule(b.knee[s], b.foot[s] + V3{0.0f, 0.0f, 0.6f}, 1.1f, 1.0f, look.cloth);
                             const V3 toe = b.foot[s] + unit(V3{1.0f, 0.0f, 0.0f}) * 1.3f;
                             fig.capsule(b.foot[s] + V3{-0.3f, 0.0f, 0.3f}, V3{toe.f, toe.l, 0.4f}, 0.9f, 0.8f, look.boots);
                             fig.capsule(b.hip[s], b.knee[s], 1.45f, 1.25f, {look.team}, 0.45f, 0.62f);
                             fig.end();
                         }});
    }
    // The body: the jacket, the plate carrier over it, its pouches in front.
    const V3 axis = unit(b.chest - b.pelvis);
    const V3 back{-axis.z, 0.0f, axis.f};
    const V3 front = back * -1.0f;
    const bool vest = kit != Kit::Rear;
    parts.push_back({middle, [&] {
                         fig.begin();
                         fig.capsule(b.pelvis, b.chest, 2.2f, 2.5f, look.cloth);
                         fig.end();
                     }});
    if (vest) {
        parts.push_back({middle + 0.01f, [&] {
                             fig.begin();
                             const float w = kit == Kit::Assault ? 2.8f : 2.6f;
                             fig.capsule(mix(b.pelvis, b.chest, 0.22f), mix(b.pelvis, b.chest, 0.92f), w, w + 0.1f, look.vest);
                             fig.end();
                         }});
        const V3 pouch = mix(b.pelvis, b.chest, 0.36f) + front * 2.5f;
        parts.push_back({fig.depth(pouch), [&, pouch] {
                             fig.begin(1.0f, false);
                             for (const float l : {-1.1f, 0.0f, 1.1f}) fig.ball(pouch + V3{0.0f, l, 0.0f}, 0.75f, {shade(look.vest.base, 0.78f)});
                             fig.end();
                         }});
    } else {  // a belt
        parts.push_back({middle + 0.01f, [&] {
                             fig.begin(1.0f, false);
                             fig.capsule(mix(b.pelvis, b.chest, 0.08f), mix(b.pelvis, b.chest, 0.14f), 2.3f, 2.3f, {{62, 50, 38, 255}});
                             fig.end();
                         }});
    }
    // On his back: a pack, the radio and its aerial, spare rockets; his weapon slung.
    const V3 pack = mix(b.pelvis, b.chest, 0.62f) + back * 2.3f;
    if (kit == Kit::Radio || kit == Kit::Sapper || kit == Kit::Assault || kit == Kit::Rpg) {
        parts.push_back({fig.depth(pack), [&, pack] {
                             fig.begin();
                             if (kit == Kit::Rpg) {  // two rockets, their warheads up
                                 for (const float l : {-0.9f, 0.9f}) {
                                     const V3 base = pack + V3{0.0f, l, -2.0f};
                                     fig.capsule(base, base + axis * 3.6f, 0.6f, 0.6f, {kDrab});
                                     fig.ball(base + axis * 4.4f, 0.95f, {{66, 84, 52, 255}});
                                 }
                             } else {
                                 const float r = kit == Kit::Sapper ? 2.0f : kit == Kit::Radio ? 1.7f : 1.5f;
                                 fig.capsule(pack - axis * 1.2f, pack + axis * 1.4f, r, r, look.pack);
                             }
                             fig.end();
                             if (kit == Kit::Radio) {  // the whip aerial
                                 fig.begin(1.0f, false);
                                 const V3 top = pack + axis * 2.4f;
                                 fig.rod(top, top + axis * 11.0f + back * 2.2f, {30, 30, 30, 255});
                                 fig.end();
                             }
                         }});
    }
    if (b.slung && !b.none) {
        const float len = arm_of(kit).length;
        const V3 across = unit(axis * 0.8f + V3{0.0f, -0.6f, 0.0f});
        const V3 c = mix(b.pelvis, b.chest, 0.55f) + back * (kit == Kit::Mortar || kit == Kit::Ags ? 2.6f : 2.0f);
        parts.push_back({fig.depth(c) - 0.05f, [&, c, across, len] {
                             fig.begin();
                             draw_weapon(fig, kit, c - across * (len * 0.5f), c + across * (len * 0.5f), false);
                             fig.end();
                         }});
    }
    // Arms: the upper arm with his side's tape, the forearm, the hand.
    for (int s = 0; s < 2; ++s) {
        const float side = s == 0 ? -1.0f : 1.0f;
        const V3 elbow = joint(b.shoulder[s], b.hand[s], 4.1f, {-0.2f, side * 0.7f, -1.0f});
        const float dp = fig.depth(elbow);
        parts.push_back({dp, [&, s, elbow, dp] {
                             fig.begin(dim(dp));
                             fig.capsule(b.shoulder[s], elbow, 1.15f, 1.0f, look.cloth);
                             fig.capsule(elbow, b.hand[s], 0.95f, 0.85f, look.cloth);
                             fig.capsule(b.shoulder[s], elbow, 1.25f, 1.1f, {look.team}, 0.3f, 0.62f);
                             fig.end();
                         }});
        const float dh = fig.depth(b.hand[s]) + 0.3f;
        parts.push_back({dh, [&, s, dh] {
                             fig.begin(dim(dh), false);
                             fig.ball(b.hand[s], 0.85f, look.skin);
                             fig.end();
                         }});
    }
    // In his hands: the weapon; a tool; a grenade.
    if (!b.slung && !b.none && !b.tool) {
        parts.push_back({fig.depth(mix(b.butt, b.muzzle, 0.5f)), [&] {
                             fig.begin();
                             draw_weapon(fig, kit, b.butt, b.muzzle, b.bipod);
                             fig.end();
                         }});
    }
    if (b.tool) {
        parts.push_back({fig.depth(mix(b.tool_a, b.tool_b, 0.5f)), [&] {
                             fig.begin();
                             fig.rod(b.tool_a, b.tool_b, {128, 96, 60, 255});
                             const V3 d = unit(b.tool_b - b.tool_a);
                             if (b.pick) {  // a pickaxe's head across the handle's end, pointed both ways
                                 const V3 n = unit(V3{-d.z, 0.0f, d.f});
                                 fig.capsule(b.tool_b + n * 2.4f - d * 0.4f, b.tool_b, 0.4f, 0.8f, {{140, 140, 136, 255}});
                                 fig.capsule(b.tool_b, b.tool_b - n * 2.4f - d * 0.4f, 0.8f, 0.4f, {{140, 140, 136, 255}});
                             } else if (b.axe) {  // an axe's head
                                 fig.capsule(b.tool_b - V3{0.0f, 0.0f, 0.2f}, b.tool_b + V3{0.0f, 0.0f, -1.6f} + d * 0.4f, 1.0f, 0.8f,
                                             {{150, 150, 146, 255}});
                             } else {  // a spade's blade
                                 fig.capsule(b.tool_b, b.tool_b + d * 1.8f, 1.1f, 1.2f, {{126, 130, 124, 255}});
                                 if (b.dirt) fig.ball(b.tool_b + d * 1.2f + V3{0.0f, 0.0f, 0.9f}, 1.2f, {{104, 80, 56, 255}});
                             }
                             fig.end();
                         }});
    }
    if (b.log) {  // along the way he faces, on his right shoulder: its bark, its cut ends pale
        const V3 on = b.shoulder[0] + V3{0.0f, 0.1f, 1.5f};
        const V3 log_front = on + V3{6.5f, 0.0f, -0.6f};
        const V3 log_rear = on + V3{-6.0f, 0.0f, 0.4f};
        parts.push_back({fig.depth(on) + 0.2f, [&, log_front, log_rear] {
                             fig.begin();
                             fig.capsule(log_rear, log_front, 1.35f, 1.25f, {{112, 84, 58, 255}, {92, 68, 46, 255}, {134, 104, 72, 255}, true, 21u});
                             fig.end();
                             fig.begin(1.0f, false);
                             fig.ball(log_front, 1.0f, {{196, 164, 116, 255}});
                             fig.ball(log_rear, 1.05f, {{196, 164, 116, 255}});
                             fig.end();
                         }});
    }
    if (b.sack) {  // on his back, over the right shoulder: burlap, lumpy with the stone, tied at its neck
        const V3 up = unit(b.chest - b.pelvis);
        const V3 back_dir{-up.z, 0.0f, up.f};
        const V3 c = mix(b.pelvis, b.chest, 0.75f) + back_dir * 2.8f + V3{0.0f, -0.4f, 0.8f};
        parts.push_back({fig.depth(c) - 0.05f, [&, c] {
                             fig.begin();
                             fig.capsule(c + V3{0.0f, 0.0f, -1.6f}, c + V3{0.0f, 0.0f, 1.4f}, 2.6f, 2.2f, {{158, 138, 98, 255}, {130, 112, 78, 255}, {176, 156, 114, 255}, true, 7u});
                             fig.ball(c + V3{0.6f, -0.3f, 3.2f}, 1.0f, {{140, 120, 84, 255}});
                             fig.end();
                         }});
    }
    if (b.grenade) {
        parts.push_back({fig.depth(b.hand[0]) + 0.4f, [&] {
                             fig.begin(1.0f, false);
                             fig.ball(b.hand[0] + V3{0.3f, 0.0f, 0.5f}, 0.9f, {{64, 74, 50, 255}});
                             fig.end();
                         }});
    }
    // The head: the neck, the face (an eye as he turns to us), his helmet, a scout's floppy hat, a rear trooper's cap.
    const V3 look_dir = unit(V3{1.0f, 0.0f, 0.0f});
    const float dhead = fig.depth(b.head);
    parts.push_back({dhead, [&] {
                         fig.begin();
                         fig.capsule(b.chest + V3{0.0f, 0.0f, 0.4f}, b.head + V3{0.0f, 0.0f, -1.4f}, 0.9f, 0.9f, look.skin);
                         fig.ball(b.head, 2.0f, look.skin);
                         fig.end();
                     }});
    const V3 eye = b.head + look_dir * 1.7f + V3{0.0f, 0.0f, 0.1f};
    parts.push_back({fig.depth(eye), [&, eye] {
                         fig.begin(1.0f, false);
                         fig.dot(eye, {40, 30, 26, 255});
                         fig.end();
                     }});
    parts.push_back({dhead + 0.02f, [&] {
                         fig.begin();
                         if (kit == Kit::Sniper) {
                             fig.brim(b.head + V3{0.0f, 0.0f, 0.7f}, 3.4f, {118, 112, 76, 255});
                             fig.ball(b.head + V3{-0.1f, 0.0f, 1.2f}, 2.1f, {{124, 118, 80, 255}}, 0.3f);
                         } else if (kit == Kit::Rear) {
                             fig.ball(b.head + V3{-0.1f, 0.0f, 0.9f}, 2.2f, {shade(look.cloth.base, 0.92f)}, 0.4f);
                             fig.capsule(b.head + look_dir * 1.4f + V3{0.0f, 0.0f, 0.8f}, b.head + look_dir * 3.2f + V3{0.0f, 0.0f, 0.6f}, 0.6f, 0.6f,
                                         {shade(look.cloth.base, 0.7f)});
                         } else {
                             fig.ball(b.head + V3{-0.15f, 0.0f, 0.75f}, 2.6f, look.helmet, 0.5f);
                         }
                         fig.end();
                     }});

    std::stable_sort(parts.begin(), parts.end(), [](const Part& a, const Part& b) { return a.depth < b.depth; });
    for (const Part& p : parts) p.draw();
}

}  // namespace

int first_frame(Pose p) {
    int n = 0;
    for (int i = 0; i < static_cast<int>(p); ++i) n += kFrames[static_cast<size_t>(i)];
    return n;
}
int frame_count(Pose p) { return kFrames[static_cast<size_t>(p)]; }
int total_frames() { return first_frame(Pose::Count); }

Image bake(Kit kit, Color team) {
    const int frames = total_frames();
    Image img = GenImageColor(kCellW * kDirs, kCellH * frames, {0, 0, 0, 0});
    auto* out = static_cast<Color*>(img.data);
    const Look look = look_of(kit, team);
    for (int d = 0; d < kDirs; ++d) {
        // The ground angle of the column, turned to the screen (the iso view turns the ground an eighth).
        const float angle = static_cast<float>(d) * 6.2831853f / static_cast<float>(kDirs) + 0.78539816f;
        int row = 0;
        for (int p = 0; p < static_cast<int>(Pose::Count); ++p) {
            for (int i = 0; i < kFrames[static_cast<size_t>(p)]; ++i, ++row) {
                Figure fig(angle);
                draw_body(fig, kit, pose(kit, static_cast<Pose>(p), i), look);
                fig.finish(out, kCellW * kDirs, d * kCellW, row * kCellH);
            }
        }
    }
    return img;
}

Offset muzzle(Kit kit, Pose p, int frame) {
    const Body b = pose(kit, p, std::clamp(frame, 0, frame_count(p) - 1));
    return {b.muzzle.f / kPxPerTile, b.muzzle.l / kPxPerTile, b.muzzle.z};
}

}  // namespace render::soldiers
