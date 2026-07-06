# Product

## Contents
- [Printing without Temper](#printing-without-temper)
- [Printing with Temper](#printing-with-temper)
- [Summary](#summary)

---

## Printing without Temper

You start with a CAD (Computer-Aided Design) model — a 3D design built from precise curved surfaces (NURBS — Non-Uniform Rational B-Splines, the math behind those curves), not flat approximations.

To print it, you export the model to a format your slicer can read. The slicer is separate software that turns a 3D model into the layer-by-layer instructions an FDM (Fused Deposition Modeling) printer follows — melted filament extruded one layer at a time.

Common export formats are STL, OBJ, and 3MF, all of which store the model as flat triangles, discarding whatever curves the CAD model actually had. STEP is the exception: it preserves the real curved surfaces as BRep (Boundary Representation — a solid stored as its bounding faces/edges/vertices, not a mesh), though the slicer still triangulates it before slicing.

Then you print, and problems can show up that were never visible in the CAD viewport:

- **Overhangs** sag or curl, because there's no material underneath to support the extruded plastic while it's still soft.
- **Small, tightly-looped sections** warp or blob, because the nozzle loops back before that plastic has had time to cool.
- **Sharp corners** over-extrude slightly, because the nozzle decelerates into the corner faster than the extrusion rate compensates for.
- **Tall, thin features** wobble during printing or snap under load later, the way a thin column buckles before a thick one.
- **The part shrinks unevenly as it cools** and drifts off its CAD dimensions, and mating holes come out mis-sized independently of that shrinkage.
- **A part larger than the printer's bed** doesn't fit and needs to be split into pieces first.
- Solid interior material burns extra filament and print time for little structural benefit.

None of this shows up until the print finishes — or fails partway through — and fixing it usually means guessing at a parameter, reprinting, and checking again.

## Printing with Temper

**Temper** is the tool that catches these problems before they reach the printer, and gives you levers to fix or optimize the model. Walking through the same steps:

**Getting the model in.** Import reads STL, OBJ, STEP, and 3MF. STEP comes in as native curved BRep geometry — no precision lost. Mesh formats (STL/OBJ/3MF) get reconstructed into flat faces wherever triangles are coplanar; recovering real curves from a mesh (fitting NURBS back onto the tessellation) is a planned enhancement, not there yet.

Whatever comes in gets healed — small gaps or orientation issues are repaired automatically — before anything else touches it.

**Catching problems before they print.** Once the model is in, Analysis runs a set of checks, each tied to one of the physical failure modes above:

- **Overhang** flags unsupported regions.
- **Not Enough Space** flags loops and corners that will reheat or over-extrude.
- **Instability** flags tall, thin sections at risk of toppling.
- **Layer Difference** flags where warping-driven shrinkage is large enough to matter.
- **Build Volume** flags a part that's simply too big for the bed, and links directly to Split (a planned tool) to divide it into printable pieces.

Every flagged problem points at *where* on the part it is, so you can see it directly instead of just being told something's wrong.

A planned Orient tool will search across bed orientations and recommend whichever one minimizes these problems automatically.

**Fixing dimensional drift.** Calibrate takes two caliper measurements from a test print — one on a flat span, one on a hole — and computes two independent corrections: a shrinkage scale factor and a hole-specific offset, since holes and flat spans drift differently.

A planned Tolerance tool will extend this further, letting you tag faces as press-fit or smooth-motion-fit and apply different corrections per surface.

**Cutting weight without cutting strength.** Structure removes the low-value interior material from over-built panels and replaces it with a diagonal strut lattice, redirecting load into a form the material carries more efficiently — the same principle a truss uses. This cuts filament and print time without weakening the part.

**Getting the model out.** Export writes the finished geometry back out, mirroring Import's formats — and re-runs Analysis first, so you don't export straight into a problem it already knows how to catch.

## Summary

Temper is for FDM users who print functional parts and iterate often — where "looks right in CAD" isn't the same as "will print successfully." It diagnoses printability problems from their actual physical cause, locates them on the model, and provides tools to fix or optimize before slicing.

It stays on the model side only: it isn't a slicer, and doesn't need to know about your specific printer.

It's also deliberately not trying to be everything:

- Printer-specific configuration is out of scope — that would mean building a slicer.
- Suggesting fixes for arbitrary flagged problems is out of scope; only Split addresses the one problem (oversized parts) that has an unambiguous fix.
