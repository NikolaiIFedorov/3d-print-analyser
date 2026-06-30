#include "Surface.hpp"
#include <BRep_Tool.hxx>
#include <BRepTools.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <GeomLProp_SLProps.hxx>
#include <Geom_Surface.hxx>

bool OcctSurface::IsPlanar() const
{
    BRepAdaptor_Surface adapt(face);
    return adapt.GetType() == GeomAbs_Plane;
}

glm::dvec3 OcctSurface::GetNormal(double u, double v) const
{
    BRepAdaptor_Surface adapt(face);
    if (adapt.GetType() == GeomAbs_Plane)
    {
        gp_Dir normal = adapt.Plane().Axis().Direction();
        if (face.Orientation() == TopAbs_REVERSED)
            normal.Reverse();
        return glm::dvec3(normal.X(), normal.Y(), normal.Z());
    }

    Handle(Geom_Surface) surf = BRep_Tool::Surface(face);
    if (surf.IsNull())
        return glm::dvec3(0, 0, 1);

    GeomLProp_SLProps props(surf, u, v, 1, 1e-6);
    if (props.IsNormalDefined())
    {
        gp_Dir normal = props.Normal();
        if (face.Orientation() == TopAbs_REVERSED)
            normal.Reverse();
        return glm::dvec3(normal.X(), normal.Y(), normal.Z());
    }
    return glm::dvec3(0, 0, 1);
}

glm::dvec3 OcctSurface::GetNormal() const
{
    BRepAdaptor_Surface adapt(face);
    if (adapt.GetType() == GeomAbs_Plane)
        return GetNormal(0.0, 0.0);

    double uMin, uMax, vMin, vMax;
    BRepTools::UVBounds(face, uMin, uMax, vMin, vMax);
    return GetNormal((uMin + uMax) / 2.0, (vMin + vMax) / 2.0);
}
