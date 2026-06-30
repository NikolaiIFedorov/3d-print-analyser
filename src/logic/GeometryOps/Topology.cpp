#include "GeometryOps/Topology.hpp"

#include "utils/log.hpp"

#include <BRepCheck_Analyzer.hxx>
#include <Standard_Boolean.hxx>

#include <string>

namespace GeometryOps
{

bool ValidateOutlineShapeTopology(const TopoDS_Shape &shape, const char *context)
{
    if (shape.IsNull())
        return false;
    BRepCheck_Analyzer analyzer(shape, Standard_True);
    if (!analyzer.IsValid())
    {
        Log::Background(std::string("GeometryOps outline: ") + context +
                        " produced a topologically invalid shape (BRepCheck_Analyzer)");
        return false;
    }
    return true;
}

} // namespace GeometryOps
