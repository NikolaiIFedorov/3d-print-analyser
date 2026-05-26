#include <TopoDS_Shape.hxx>
#include <functional>

int main() {
    TopoDS_Shape s;
    std::hash<TopoDS_Shape> hasher;
    hasher(s);
    return 0;
}
