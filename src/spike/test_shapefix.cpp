#include <ShapeFix_Shape.hxx>
#include <TopoDS_Shape.hxx>

int main() {
    TopoDS_Shape s;
    ShapeFix_Shape fixer(s);
    fixer.Perform();
    TopoDS_Shape fixed = fixer.Shape();
    return 0;
}
