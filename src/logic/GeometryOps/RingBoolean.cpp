#include "RingBoolean.hpp"
#include "WireOps.hpp"

#include "utils/log.hpp"

#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepTools.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Iterator.hxx>
#include <TopoDS_Wire.hxx>

namespace
{

/// Fuses a set of rings (already split by outer/hole) into one OCCT face shape. Null if `rings`
/// is empty.
TopoDS_Shape FuseRingsToFaces(const std::vector<std::vector<glm::dvec3>> &rings, double zBottom)
{
    TopoDS_Shape result;
    bool haveFirst = false;
    for (const auto &ring : rings)
    {
        if (ring.size() < 3)
            continue;
        const TopoDS_Wire wire = GeometryOps::MakeFlatWireFromRing(ring, zBottom);
        if (wire.IsNull())
            continue;
        BRepBuilderAPI_MakeFace mkFace(wire);
        if (!mkFace.IsDone())
            continue;

        if (!haveFirst)
        {
            result = mkFace.Face();
            haveFirst = true;
            continue;
        }

        BRepAlgoAPI_Fuse fuse(result, mkFace.Face());
        fuse.Build();
        if (fuse.IsDone())
            result = fuse.Shape();
    }
    return result;
}

/// Builds the full multi-island, holes-cut-out shape for one layer's classified rings: fuses all
/// outer-loop faces, fuses all hole-loop faces, then cuts the hole union from the outer union.
/// Island/hole pairing doesn't need to be known — cutting the fused hole union from the fused
/// outer union removes exactly the right material everywhere, since a hole only geometrically
/// overlaps the outer face it's actually nested in.
TopoDS_Shape BuildLayerShape(const std::vector<GeometryOps::ClassifiedRing> &layer, double zBottom)
{
    std::vector<std::vector<glm::dvec3>> outerRings, holeRings;
    for (const auto &cr : layer)
        (cr.isHole ? holeRings : outerRings).push_back(cr.ring);

    const TopoDS_Shape outerUnion = FuseRingsToFaces(outerRings, zBottom);
    if (outerUnion.IsNull())
        return TopoDS_Shape();

    if (holeRings.empty())
        return outerUnion;

    const TopoDS_Shape holeUnion = FuseRingsToFaces(holeRings, zBottom);
    if (holeUnion.IsNull())
        return TopoDS_Shape(); // hole rings existed but failed to build — don't guess, signal failure

    BRepAlgoAPI_Cut cut(outerUnion, holeUnion);
    cut.SetFuzzyValue(1e-3);
    cut.Build();
    // Fail-safe to null rather than the uncut outer shape: silently returning the outer area
    // with its holes still "filled in" would overstate area/coverage just as wrongly as
    // `RingDifference`'s analogous fallback below, just less dramatically.
    return cut.IsDone() ? cut.Shape() : TopoDS_Shape();
}

} // namespace

namespace GeometryOps
{

std::vector<ClassifiedRing> RingDifference(const std::vector<ClassifiedRing> &a,
                                           const std::vector<ClassifiedRing> &b, double zBottom,
                                           double chordTolMm)
{
    std::vector<ClassifiedRing> result;

    const TopoDS_Shape shapeA = BuildLayerShape(a, zBottom);
    if (shapeA.IsNull())
        return result;

    TopoDS_Shape diff = shapeA;
    TopoDS_Shape shapeB;
    if (!b.empty())
    {
        // `b` has rings, so a null/failed shape here is a genuine build failure, not "nothing to
        // subtract" — falling back to `diff = shapeA` in that case would silently claim the
        // entire current cross-section is new/unsupported, which is a far more dramatic (and
        // wrong) result than simply skipping this layer pair.
        shapeB = BuildLayerShape(b, zBottom);
        if (shapeB.IsNull())
            return result;

        BRepAlgoAPI_Cut cut(shapeA, shapeB);
        cut.SetFuzzyValue(1e-3);
        cut.Build();
        if (!cut.IsDone())
            return result;
        diff = cut.Shape();
    }

    if (diff.IsNull())
        return result;

    // Outer boundary rings — one outer wire per result face.
    for (TopExp_Explorer faceExp(diff, TopAbs_FACE); faceExp.More(); faceExp.Next())
    {
        const TopoDS_Face &face = TopoDS::Face(faceExp.Current());
        const TopoDS_Wire outerWire = BRepTools::OuterWire(face);
        if (outerWire.IsNull())
            continue;
        std::vector<glm::dvec3> ring = DiscretizeWireToXyRing(outerWire, zBottom, chordTolMm);
        if (ring.size() >= 3)
            result.push_back({std::move(ring), false});
    }

    // Clipped hole rings: B ∩ A = B - (B - A).
    // BRepAlgoAPI_Common returns 0 faces for flat-face intersections (targets 3D solids only).
    // Two Cuts are equivalent and work correctly. Special case: when all holes are fully inside
    // the plate, B - A = ∅ and OCCT's Cut(B, ∅) doesn't reliably return B, so we detect that
    // case (0 faces from cut1) and emit shapeB directly instead of doing the second cut.
    if (!b.empty() && !shapeB.IsNull())
    {
        // cut1 = B - A = portions of holes outside the outer boundary
        BRepAlgoAPI_Cut cut1(shapeB, shapeA);
        cut1.SetFuzzyValue(1e-3);
        cut1.Build();

        const TopoDS_Shape *holeSource = nullptr;  // the shape whose faces become hole rings
        TopoDS_Shape cut2Shape;

        if (cut1.IsDone())
        {
            int cut1Faces = 0;
            for (TopExp_Explorer e(cut1.Shape(), TopAbs_FACE); e.More(); e.Next())
                ++cut1Faces;

            if (cut1Faces == 0)
            {
                // All holes are fully inside the outer — B ∩ A = B. Use shapeB directly.
                holeSource = &shapeB;
            }
            else
            {
                // Some holes extend outside — cut2 = B - cut1 = B ∩ A.
                BRepAlgoAPI_Cut cut2(shapeB, cut1.Shape());
                cut2.SetFuzzyValue(1e-3);
                cut2.Build();
                if (cut2.IsDone() && !cut2.Shape().IsNull())
                {
                    cut2Shape = cut2.Shape();
                    holeSource = &cut2Shape;
                }
            }
        }

        if (holeSource != nullptr)
        {
            for (TopExp_Explorer faceExp(*holeSource, TopAbs_FACE); faceExp.More(); faceExp.Next())
            {
                const TopoDS_Face &face = TopoDS::Face(faceExp.Current());

                // BRepTools::OuterWire returns null for REVERSED-orientation faces (which is what
                // FuseRingsToFaces produces for CW-wound wires). Fall back to the first wire in
                // the face's direct children so we can still discretize the hole circle.
                TopoDS_Wire wire = BRepTools::OuterWire(face);
                if (wire.IsNull())
                {
                    for (TopoDS_Iterator it(face, Standard_False, Standard_False); it.More(); it.Next())
                    {
                        if (it.Value().ShapeType() == TopAbs_WIRE)
                        {
                            wire = TopoDS::Wire(it.Value());
                            break;
                        }
                    }
                }
                if (wire.IsNull())
                    continue;

                std::vector<glm::dvec3> ring = DiscretizeWireToXyRing(wire, zBottom, chordTolMm);
                if (ring.size() >= 3)
                    result.push_back({std::move(ring), true});
            }
        }
    }

    return result;
}

std::vector<std::vector<glm::dvec3>> ClipHolesToOuter(const std::vector<ClassifiedRing> &a,
                                                       const std::vector<ClassifiedRing> &b,
                                                       double zBottom, double chordTolMm)
{
    std::vector<std::vector<glm::dvec3>> result;

    std::vector<std::vector<glm::dvec3>> outerRings;
    outerRings.reserve(a.size());
    for (const auto &cr : a)
        outerRings.push_back(cr.ring);
    const TopoDS_Shape outerShape = FuseRingsToFaces(outerRings, zBottom);
    if (outerShape.IsNull())
        return result;

    for (const auto &cr : b)
    {
        if (cr.ring.size() < 3)
            continue;
        const TopoDS_Wire wire = GeometryOps::MakeFlatWireFromRing(cr.ring, zBottom);
        if (wire.IsNull())
            continue;
        BRepBuilderAPI_MakeFace mkFace(wire);
        if (!mkFace.IsDone())
            continue;

        BRepAlgoAPI_Common common(outerShape, mkFace.Face());
        common.SetFuzzyValue(1e-3);
        common.Build();
        if (!common.IsDone() || common.Shape().IsNull())
        {
            // Boolean failed — fall back to the unclipped hole ring so we show something.
            result.push_back(cr.ring);
            continue;
        }

        bool anyFace = false;
        for (TopExp_Explorer faceExp(common.Shape(), TopAbs_FACE); faceExp.More(); faceExp.Next())
        {
            anyFace = true;
            const TopoDS_Face &face = TopoDS::Face(faceExp.Current());
            const TopoDS_Wire outerWire = BRepTools::OuterWire(face);
            if (outerWire.IsNull())
                continue;
            std::vector<glm::dvec3> ring = DiscretizeWireToXyRing(outerWire, zBottom, chordTolMm);
            LOG_SESSION("ClipHolesToOuter", "clipped hole pts", ring.size(), "z", zBottom);
            if (ring.size() >= 3)
                result.push_back(std::move(ring));
        }
        if (!anyFace)
            LOG_SESSION("ClipHolesToOuter", "hole fully outside outer — skipped", "z", zBottom);
    }
    return result;
}

} // namespace GeometryOps
