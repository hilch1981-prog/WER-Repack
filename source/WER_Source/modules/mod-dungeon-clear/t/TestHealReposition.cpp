/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include <cmath>

#include "DungeonClearMath.h"

using DungeonClearMath::HealCandidate;
using DungeonClearMath::HealTargetNone;
using DungeonClearMath::SelectHealTarget;
using DungeonClearMath::HealStandoffCandidates;

namespace
{
    constexpr float kFloor = 90.0f;
    constexpr float kBias = 15.0f;
}

// Nobody below the floor -> no target.
TEST(DungeonClearHealRepositionTest, AllHealthyNoTarget)
{
    std::vector<HealCandidate> m = {
        { 100.0f, false }, { 95.0f, true }, { 92.0f, false }
    };
    EXPECT_EQ(SelectHealTarget(m, kFloor, kBias), HealTargetNone);
}

// A single hurt member is picked.
TEST(DungeonClearHealRepositionTest, SingleHurtPicked)
{
    std::vector<HealCandidate> m = {
        { 100.0f, false }, { 40.0f, false }, { 95.0f, true }
    };
    EXPECT_EQ(SelectHealTarget(m, kFloor, kBias), 1u);
}

// Tank bias breaks the pick toward the tank when both need healing and are
// close in health (tank 85 vs dps 80: 85-15=70 < 80).
TEST(DungeonClearHealRepositionTest, TankBiasFavoursTank)
{
    std::vector<HealCandidate> m = {
        { 80.0f, false },  // dps, idx 0
        { 85.0f, true }    // tank, idx 1
    };
    EXPECT_EQ(SelectHealTarget(m, kFloor, kBias), 1u);
}

// A HEALTHY tank above the floor never steals the pick from a hurt dps — the
// "needs healing" gate is on raw health, applied before the bias.
TEST(DungeonClearHealRepositionTest, HealthyTankNeverStealsPick)
{
    std::vector<HealCandidate> m = {
        { 80.0f, false },  // dps, idx 0 (hurt)
        { 95.0f, true }    // tank, idx 1 (above floor -> excluded)
    };
    EXPECT_EQ(SelectHealTarget(m, kFloor, kBias), 0u);
}

// A clearly more-hurt dps still beats the tank even with the bias.
TEST(DungeonClearHealRepositionTest, MuchLowerDpsBeatsTank)
{
    std::vector<HealCandidate> m = {
        { 20.0f, false },  // dps, idx 0
        { 88.0f, true }    // tank, idx 1 (88-15=73 > 20)
    };
    EXPECT_EQ(SelectHealTarget(m, kFloor, kBias), 0u);
}

// Empty input is handled.
TEST(DungeonClearHealRepositionTest, EmptyNoTarget)
{
    std::vector<HealCandidate> m;
    EXPECT_EQ(SelectHealTarget(m, kFloor, kBias), HealTargetNone);
}

// Candidate count is ringPoints + 1.
TEST(DungeonClearHealRepositionTest, StandoffCount)
{
    Position target(100.0f, 100.0f, 50.0f, 0.0f);
    Position bot(80.0f, 100.0f, 50.0f, 0.0f);
    auto pts = HealStandoffCandidates(target, bot, 10.0f, 7);
    EXPECT_EQ(pts.size(), 8u);
}

// Every candidate lies on the standoff circle around the target.
TEST(DungeonClearHealRepositionTest, StandoffOnCircle)
{
    Position target(100.0f, 100.0f, 50.0f, 0.0f);
    Position bot(60.0f, 130.0f, 50.0f, 0.0f);
    float const r = 12.0f;
    auto pts = HealStandoffCandidates(target, bot, r, 7);
    for (Position const& p : pts)
    {
        float const dx = p.GetPositionX() - target.GetPositionX();
        float const dy = p.GetPositionY() - target.GetPositionY();
        EXPECT_NEAR(std::sqrt(dx * dx + dy * dy), r, 1e-2f);
    }
}

// First candidate is on the bot's side of the target (shortest reposition):
// it is the closest of all candidates to the bot.
TEST(DungeonClearHealRepositionTest, StandoffFirstIsBotSide)
{
    Position target(100.0f, 100.0f, 50.0f, 0.0f);
    Position bot(70.0f, 100.0f, 50.0f, 0.0f);  // due -X of target
    auto pts = HealStandoffCandidates(target, bot, 10.0f, 7);
    ASSERT_FALSE(pts.empty());
    // First candidate should sit between target and bot (x ~ 90).
    EXPECT_NEAR(pts[0].GetPositionX(), 90.0f, 1e-2f);
    EXPECT_NEAR(pts[0].GetPositionY(), 100.0f, 1e-2f);

    float const fdx = pts[0].GetPositionX() - bot.GetPositionX();
    float const fdy = pts[0].GetPositionY() - bot.GetPositionY();
    float const firstDist = std::sqrt(fdx * fdx + fdy * fdy);
    for (std::size_t i = 1; i < pts.size(); ++i)
    {
        float const dx = pts[i].GetPositionX() - bot.GetPositionX();
        float const dy = pts[i].GetPositionY() - bot.GetPositionY();
        EXPECT_LE(firstDist, std::sqrt(dx * dx + dy * dy) + 1e-3f);
    }
}

// Degenerate bot-on-target input falls back to +X without NaNs.
TEST(DungeonClearHealRepositionTest, StandoffDegenerate)
{
    Position target(100.0f, 100.0f, 50.0f, 0.0f);
    Position bot(100.0f, 100.0f, 50.0f, 0.0f);
    auto pts = HealStandoffCandidates(target, bot, 10.0f, 7);
    ASSERT_EQ(pts.size(), 8u);
    EXPECT_NEAR(pts[0].GetPositionX(), 110.0f, 1e-2f);
    EXPECT_NEAR(pts[0].GetPositionY(), 100.0f, 1e-2f);
}

// --- the close-on-target fallback, and the ceiling clip it used to author ----
//
// Regression cover for tp-20260828-171530-1 (Blackwing Lair, 5 of 5 runs): the
// fallback interpolated x and y toward the heal target but left z at the
// TARGET's height, naming a point on the target's floor over the bot's own x/y.
// DcMoveTo's ground-snap declines corrections past 3yd (NavmeshSnap's vertical
// extent spans a storey), stock's z-search refused the move, and the
// exact-waypoint retry overrode the refusal — so healers walked up through a
// solid ceiling into a 14-mob formation 24.8yd overhead.

// The fallback point stays ON the bot->target line: z interpolates with x and y.
TEST(DungeonClearHealRepositionTest, FallbackInterpolatesZWithXY)
{
    Position bot(0.0f, 0.0f, 100.0f, 0.0f);
    Position target(0.0f, 20.0f, 120.0f, 0.0f);  // 20yd out, 20yd up
    Position const p = DungeonClearMath::HealCloseFallbackPoint(bot, target, 5.0f);

    // Stops 5yd short of 20 -> 3/4 of the way along, in EVERY coordinate.
    EXPECT_NEAR(p.GetPositionY(), 15.0f, 1e-3f);
    EXPECT_NEAR(p.GetPositionZ(), 115.0f, 1e-3f);
}

// A same-floor target is unaffected: flat ground in, flat ground out. This is
// the overwhelmingly common case and it must not move.
TEST(DungeonClearHealRepositionTest, FallbackFlatGroundUnchanged)
{
    Position bot(0.0f, 0.0f, 100.0f, 0.0f);
    Position target(0.0f, 20.0f, 100.0f, 0.0f);
    Position const p = DungeonClearMath::HealCloseFallbackPoint(bot, target, 5.0f);

    EXPECT_NEAR(p.GetPositionY(), 15.0f, 1e-3f);
    EXPECT_NEAR(p.GetPositionZ(), 100.0f, 1e-3f);
}

// THE BUG. Real geometry from tr-20260828-171538-5: a healer standing in the
// Broodlord approach hall at z 424.5 whose heal target has already reached the
// drake hall at z 449.3, 20yd away in plan view (Simino's own reposition logged
// 12.5yd, the tank's 23.0yd). The old code handed back z 449.3 for a point only
// three quarters of the way there — the target's floor over the hall's x/y.
TEST(DungeonClearHealRepositionTest, FallbackNeverAuthorsTheTargetsFloor)
{
    Position bot(-7523.8f, -975.0f, 424.5f, 0.0f);
    Position target(-7523.8f, -955.0f, 449.3f, 0.0f);  // 20yd out, 24.8yd up
    Position const p = DungeonClearMath::HealCloseFallbackPoint(bot, target, 5.0f);

    // Strictly between the two floors, never ON the target's.
    EXPECT_GT(p.GetPositionZ(), 424.5f);
    EXPECT_LT(p.GetPositionZ(), 449.3f);
    // 3/4 of the way along the line: 424.5 + 0.75 * 24.8.
    EXPECT_NEAR(p.GetPositionZ(), 443.1f, 1e-1f);

    // And the movement layer will not force it on the fast path — this
    // destination is a storey off the bot, so it has to be probed for a route
    // before the exact-waypoint retry may override stock's refusal.
    EXPECT_FALSE(DungeonClearMath::MayRetryExactWaypoint(
        p.GetPositionZ(), bot.GetPositionZ(), 5.0f));
}

// Inside minGap the target itself is returned, untouched.
TEST(DungeonClearHealRepositionTest, FallbackInsideGapReturnsTarget)
{
    Position bot(0.0f, 0.0f, 100.0f, 0.0f);
    Position target(0.0f, 3.0f, 101.0f, 0.0f);
    Position const p = DungeonClearMath::HealCloseFallbackPoint(bot, target, 5.0f);

    EXPECT_NEAR(p.GetPositionY(), 3.0f, 1e-3f);
    EXPECT_NEAR(p.GetPositionZ(), 101.0f, 1e-3f);
}

// Degenerate bot-on-target input is the target, with no NaN from the divide.
TEST(DungeonClearHealRepositionTest, FallbackDegenerateNoNaN)
{
    Position bot(50.0f, 50.0f, 10.0f, 0.0f);
    Position target(50.0f, 50.0f, 10.0f, 0.0f);
    Position const p = DungeonClearMath::HealCloseFallbackPoint(bot, target, 5.0f);

    EXPECT_FALSE(std::isnan(p.GetPositionX()));
    EXPECT_FALSE(std::isnan(p.GetPositionZ()));
    EXPECT_NEAR(p.GetPositionZ(), 10.0f, 1e-3f);
}

// --- the exact-waypoint retry's level gate ----------------------------------
//
// This is the CHEAP HALF of the gate: true means "safe, no probe needed". False
// is not a refusal — DcMoveTo then asks DcEngageGeometry::IsPointLevelReachable
// whether a route actually arrives at the destination, which is what keeps ramps
// and stair flights (legitimately off-level, legitimately routable) on the retry.
// That half needs a navmesh and is covered by the fixture-gated nav probes.

// The cases the retry exists for — a legitimate destination a few yards away on
// the bot's own floor — take the fast path with no probe at all.
TEST(DungeonClearHealRepositionTest, RetryAllowedOnOwnLevel)
{
    // Xomja, refused 45x on a destination 1.7yd away.
    EXPECT_TRUE(DungeonClearMath::MayRetryExactWaypoint(100.0f, 100.0f, 5.0f));
    EXPECT_TRUE(DungeonClearMath::MayRetryExactWaypoint(101.7f, 100.0f, 5.0f));
    // A ramp or stair step, either direction, right up to the tolerance.
    EXPECT_TRUE(DungeonClearMath::MayRetryExactWaypoint(104.9f, 100.0f, 5.0f));
    EXPECT_TRUE(DungeonClearMath::MayRetryExactWaypoint(95.1f, 100.0f, 5.0f));
}

// A destination on another storey does not take the fast path — in either
// direction — so it reaches the reachability probe rather than being forced.
// Up is the BWL ceiling; down is the same trick over a ledge.
TEST(DungeonClearHealRepositionTest, RetryNeedsAProbeAcrossLevels)
{
    // The drake hall over the Broodlord approach: z 449.3 asked from z 424.5.
    EXPECT_FALSE(DungeonClearMath::MayRetryExactWaypoint(449.3f, 424.5f, 5.0f));
    EXPECT_FALSE(DungeonClearMath::MayRetryExactWaypoint(424.5f, 449.3f, 5.0f));
    // And just past the tolerance, so the boundary is pinned.
    EXPECT_FALSE(DungeonClearMath::MayRetryExactWaypoint(105.1f, 100.0f, 5.0f));
}
