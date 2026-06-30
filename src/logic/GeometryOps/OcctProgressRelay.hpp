#pragma once

#include <Message_ProgressIndicator.hxx>
#include <Message_ProgressScope.hxx>
#include <Standard_Handle.hxx>

#include <functional>
#include <utility>

namespace GeometryOps
{

/// Adapts OCCT's Message_ProgressIndicator to a plain `void(double)` callback so long-running
/// OCCT algorithms that accept a Message_ProgressRange (BRepBuilderAPI_Sewing::Perform,
/// BRepMesh_IncrementalMesh::Perform, BRepAlgoAPI_BooleanOperation::Build, ...) can report their
/// internal progress (0..1) through this app's existing UI progress callbacks instead of only
/// signalling "started" / "finished" around the whole call.
class OcctProgressRelay : public Message_ProgressIndicator
{
public:
    DEFINE_STANDARD_RTTI_INLINE(OcctProgressRelay, Message_ProgressIndicator)

    explicit OcctProgressRelay(std::function<void(double)> onUpdate) : myOnUpdate(std::move(onUpdate)) {}

protected:
    void Show(const Message_ProgressScope &, const Standard_Boolean) override
    {
        if (myOnUpdate)
            myOnUpdate(GetPosition());
    }

private:
    std::function<void(double)> myOnUpdate;
};

} // namespace GeometryOps
