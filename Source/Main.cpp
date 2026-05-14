#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_osc/juce_osc.h>
#include "FsmModel.h"
#include "SuperColliderHost.h"

#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>

namespace
{
juce::Colour backgroundTop() { return juce::Colour (0xff111318); }
juce::Colour backgroundBottom() { return juce::Colour (0xff20242b); }
juce::Colour ink() { return juce::Colour (0xfff2efe7); }
juce::Colour mutedInk() { return juce::Colour (0xffaeb5bd); }
juce::Colour accentA() { return juce::Colour (0xffffc857); }
juce::Colour accentB() { return juce::Colour (0xff52d1dc); }
juce::Colour accentC() { return juce::Colour (0xfff76f8e); }
juce::Colour selectedFill() { return juce::Colour (0xff3a3320); }
juce::Colour inspectedFill() { return juce::Colour (0xff1c3139); }
juce::Colour selectedRow() { return juce::Colour (0xff34313a); }

juce::Colour paletteColour (int index)
{
    static constexpr juce::uint32 colours[] =
    {
        0xff52d1dc, 0xffffc857, 0xfff76f8e, 0xff7bd88f,
        0xffb48cff, 0xffff9f68, 0xff64b5f6, 0xfff06292
    };

    return juce::Colour (colours[static_cast<size_t> (index) % std::size (colours)]);
}
} // namespace

class GraphComponent final : public juce::Component,
                             private juce::Timer
{
public:
    std::function<void (int)> onStateChosen;
    std::function<void (int)> onNestedBadgeChosen;
    std::function<void (int, int)> onNestedStateChosen;
    std::function<void (int, int, int)> onSecondLayerNestedStateChosen;
    std::function<void (int, int)> onNestedStateCountChanged;
    std::function<void (int, int, int)> onSecondLayerNestedStateCountChanged;

    void setInspectedMachine (MachineModel* inspected)
    {
        inspectedMachine = inspected;
        repaint();
    }

    explicit GraphComponent (MachineModel& machineToUse) : machine (&machineToUse)
    {
        setWantsKeyboardFocus (true);
        startTimerHz (30);
    }

    void setMachine (MachineModel& machineToUse)
    {
        if (machine != &machineToUse)
        {
            manualNodeOffsets.clear();
            clearNodePositionLock();
            fitView();
        }

        machine = &machineToUse;
        repaint();
    }

    void fitView()
    {
        zoom = 1.0f;
        panOffset = {};
        repaint();
    }

    void resetLayout()
    {
        finishNestedStateCountEdit (false);
        ensureManualOffsetSize();
        std::fill (manualNodeOffsets.begin(), manualNodeOffsets.end(), juce::Point<float> {});
        clearNodePositionLock();
        fitView();
    }

    void beginNodePositionLock()
    {
        finishNestedStateCountEdit (false);
        nodePositionLockActive = false;
        layoutStatesNormal (true, true);
        lockedScreenPositions = statePositions;
        lockedScreenRadius = stateRadius;
        nodePositionLockActive = ! lockedScreenPositions.empty();
    }

    void endNodePositionLock()
    {
        if (! nodePositionLockActive)
            return;

        auto lockedPositions = lockedScreenPositions;
        clearNodePositionLock();

        layoutStatesNormal (false, false);
        auto basePositions = statePositions;
        ensureManualOffsetSize();

        const auto count = juce::jmin (static_cast<int> (basePositions.size()),
                                       static_cast<int> (lockedPositions.size()));
        for (int i = 0; i < count; ++i)
            manualNodeOffsets[static_cast<size_t> (i)] = screenToGraph (lockedPositions[static_cast<size_t> (i)])
                                                       - basePositions[static_cast<size_t> (i)];

        repaint();
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        const auto keyCode = key.getKeyCode();

        if (keyCode == 'f' || keyCode == 'F')
        {
            fitView();
            return true;
        }

        if (keyCode == 'r' || keyCode == 'R')
        {
            resetLayout();
            return true;
        }

        return false;
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();
        juce::ColourGradient bg (backgroundTop(), bounds.getTopLeft(), backgroundBottom(), bounds.getBottomRight(), false);
        g.setGradientFill (bg);
        g.fillRoundedRectangle (bounds.reduced (2.0f), 8.0f);

        layoutStates();
        drawRules (g);
        drawStates (g);
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        grabKeyboardFocus();
        dragStart = event.position;
        panStart = panOffset;
        draggingStateIndex = -1;
        draggedState = false;

        if (event.mods.isPopupMenu() || event.mods.isMiddleButtonDown() || event.mods.isAltDown())
            return;

        for (int i = 0; i < static_cast<int> (statePositions.size()); ++i)
        {
            if (auto* child = machine->childMachine (i))
            {
                const auto secondLayerBadge = hitTestSecondLayerBadge (*child, statePositions[static_cast<size_t> (i)], event.position);
                if (secondLayerBadge >= 0)
                {
                    if (auto* grandchild = child->childMachine (secondLayerBadge))
                        if (onSecondLayerNestedStateChosen)
                            onSecondLayerNestedStateChosen (i, secondLayerBadge, grandchild->selectedState);
                    return;
                }

                const auto secondLayerState = hitTestSecondLayerNestedState (*child, statePositions[static_cast<size_t> (i)], event.position);
                if (secondLayerState.first >= 0 && secondLayerState.second >= 0)
                {
                    if (onSecondLayerNestedStateChosen)
                        onSecondLayerNestedStateChosen (i, secondLayerState.first, secondLayerState.second);
                    return;
                }

                const auto childState = hitTestNestedState (*child, statePositions[static_cast<size_t> (i)], event.position);
                if (childState >= 0)
                {
                    if (onNestedStateChosen)
                        onNestedStateChosen (i, childState);
                    return;
                }
            }
        }

        for (int i = 0; i < static_cast<int> (statePositions.size()); ++i)
        {
            if (machine->childMachine (i) != nullptr
                && getNestedBadgeBounds (*machine->childMachine (i), statePositions[static_cast<size_t> (i)]).contains (event.position))
            {
                if (onNestedBadgeChosen)
                    onNestedBadgeChosen (i);
                return;
            }
        }

        for (int i = 0; i < static_cast<int> (statePositions.size()); ++i)
        {
            if (statePositions[static_cast<size_t> (i)].getDistanceFrom (event.position) < stateRadius)
            {
                ensureManualOffsetSize();
                draggingStateIndex = i;
                nodeOffsetStart = manualNodeOffsets[static_cast<size_t> (i)];
                machine->selectedState = i;
                machine->selectedLane = 0;
                if (onStateChosen)
                    onStateChosen (i);
                repaint();
                return;
            }
        }

    }

    void mouseDrag (const juce::MouseEvent& event) override
    {
        const auto delta = event.position - dragStart;
        if (delta.getDistanceFromOrigin() < 2.0f)
            return;

        if (draggingStateIndex >= 0)
        {
            ensureManualOffsetSize();
            const auto graphDelta = delta / juce::jmax (0.001f, zoom);
            manualNodeOffsets[static_cast<size_t> (draggingStateIndex)] = nodeOffsetStart + graphDelta;
            draggedState = true;
        }
        else
        {
            panOffset = panStart + delta;
        }

        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        draggingStateIndex = -1;
        draggedState = false;
    }

    void mouseWheelMove (const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override
    {
        const auto cursor = event.position;
        const auto centre = getLocalBounds().toFloat().getCentre();
        const auto before = screenToGraph (cursor);
        const auto wheelDelta = std::abs (wheel.deltaY) > std::abs (wheel.deltaX) ? wheel.deltaY : wheel.deltaX;
        const auto zoomFactor = std::pow (1.18f, wheelDelta * 6.0f);
        const auto newZoom = juce::jlimit (0.55f, 2.8f, zoom * zoomFactor);

        if (std::abs (newZoom - zoom) < 0.001f)
            return;

        zoom = newZoom;
        panOffset = cursor - centre - ((before - centre) * zoom);
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent& event) override
    {
        for (int i = 0; i < static_cast<int> (statePositions.size()); ++i)
        {
            if (auto* child = machine->childMachine (i))
            {
                const auto badgeHit = hitTestSecondLayerBadge (*child, statePositions[static_cast<size_t> (i)], event.position);
                if (badgeHit >= 0)
                {
                    startSecondLayerStateCountEdit (i, badgeHit, getSecondLayerBadgeBounds (*child->childMachine (badgeHit),
                                                                                             getNestedStatePoint (*child, statePositions[static_cast<size_t> (i)], badgeHit)));
                    return;
                }
            }

            if (machine->childMachine (i) != nullptr
                && getNestedBadgeBounds (*machine->childMachine (i), statePositions[static_cast<size_t> (i)]).contains (event.position))
            {
                startNestedStateCountEdit (i, getNestedBadgeBounds (*machine->childMachine (i), statePositions[static_cast<size_t> (i)]));
                return;
            }
        }
    }

private:
    void layoutStates()
    {
        if (nodePositionLockActive && lockedScreenPositions.size() == static_cast<size_t> (machine->getStateCount()))
        {
            statePositions = lockedScreenPositions;
            stateRadius = lockedScreenRadius;
            return;
        }

        layoutStatesNormal (true, true);
    }

    void layoutStatesNormal (bool includeManualOffsets, bool applyViewTransform)
    {
        const auto count = machine->getStateCount();
        statePositions.resize (static_cast<size_t> (count));
        ensureManualOffsetSize();

        stateRadius = juce::jmap (static_cast<float> (count), 1.0f, static_cast<float> (maxStateCount), 54.0f, 34.0f);
        stateRadius = juce::jlimit (34.0f, 54.0f, stateRadius);

        const auto outerMargin = getOuterNodeExtent() + 28.0f;
        auto area = getLocalBounds().toFloat().reduced (outerMargin, outerMargin * 0.78f);
        const auto maxLayoutWidth = area.getHeight() * 4.25f;
        if (area.getWidth() > maxLayoutWidth)
            area = area.withSizeKeepingCentre (maxLayoutWidth, area.getHeight());

        auto centre = area.getCentre();
        auto rx = area.getWidth() * 0.50f;
        auto ry = area.getHeight() * 0.47f;

        for (int i = 0; i < count; ++i)
        {
            auto angle = (juce::MathConstants<float>::twoPi * static_cast<float> (i) / static_cast<float> (count))
                       - juce::MathConstants<float>::halfPi;
            statePositions[static_cast<size_t> (i)] = { centre.x + std::cos (angle) * rx,
                                                        centre.y + std::sin (angle) * ry };
        }

        relaxStatePositions (area);
        if (includeManualOffsets)
            applyManualNodeOffsets();

        if (applyViewTransform)
            applyViewTransformToLayout();
    }

    void clearNodePositionLock()
    {
        nodePositionLockActive = false;
        lockedScreenPositions.clear();
    }

    void ensureManualOffsetSize()
    {
        const auto count = static_cast<size_t> (machine->getStateCount());
        if (manualNodeOffsets.size() != count)
            manualNodeOffsets.resize (count, {});
    }

    void applyManualNodeOffsets()
    {
        for (int i = 0; i < static_cast<int> (statePositions.size()); ++i)
        {
            auto& p = statePositions[static_cast<size_t> (i)];
            p += manualNodeOffsets[static_cast<size_t> (i)];
        }
    }

    void applyViewTransformToLayout()
    {
        for (auto& position : statePositions)
            position = graphToScreen (position);

        stateRadius *= zoom;
    }

    juce::Point<float> graphToScreen (juce::Point<float> point) const
    {
        const auto centre = getLocalBounds().toFloat().getCentre();
        return centre + panOffset + ((point - centre) * zoom);
    }

    juce::Point<float> screenToGraph (juce::Point<float> point) const
    {
        const auto centre = getLocalBounds().toFloat().getCentre();
        return centre + ((point - centre - panOffset) / zoom);
    }

    void relaxStatePositions (juce::Rectangle<float> area)
    {
        const auto count = static_cast<int> (statePositions.size());
        if (count < 2)
            return;

        constexpr int iterations = 150;
        for (int pass = 0; pass < iterations; ++pass)
        {
            for (int a = 0; a < count; ++a)
            {
                for (int b = a + 1; b < count; ++b)
                {
                    auto& pa = statePositions[static_cast<size_t> (a)];
                    auto& pb = statePositions[static_cast<size_t> (b)];
                    auto delta = pb - pa;
                    auto distance = std::sqrt (delta.x * delta.x + delta.y * delta.y);

                    if (distance < 0.001f)
                    {
                        delta = { 1.0f, 0.0f };
                        distance = 1.0f;
                    }

                    const auto minDistance = getNodeClearance (a) + getNodeClearance (b);
                    if (distance >= minDistance)
                        continue;

                    delta.x /= distance;
                    delta.y /= distance;

                    const auto push = (minDistance - distance) * 0.68f;
                    pa -= delta * push;
                    pb += delta * push;
                }
            }

            for (int i = 0; i < count; ++i)
            {
                auto& p = statePositions[static_cast<size_t> (i)];
                const auto extent = getNodeClearance (i);
                p.x = juce::jlimit (area.getX() + extent, area.getRight() - extent, p.x);
                p.y = juce::jlimit (area.getY() + extent, area.getBottom() - extent, p.y);
            }
        }
    }

    void drawRules (juce::Graphics& g)
    {
        for (const auto& rule : machine->rules)
        {
            if (rule.from >= static_cast<int> (statePositions.size()) || rule.to >= static_cast<int> (statePositions.size()))
                continue;

            auto fromCentre = statePositions[static_cast<size_t> (rule.from)];
            auto toCentre = statePositions[static_cast<size_t> (rule.to)];
            auto direction = toCentre - fromCentre;
            const auto length = juce::jmax (1.0f, std::sqrt (direction.x * direction.x + direction.y * direction.y));
            direction.x /= length;
            direction.y /= length;
            auto from = fromCentre + direction * (stateRadius * 1.05f);
            auto to = toCentre - direction * (stateRadius * 1.05f);
            auto mid = (from + to) * 0.5f;
            auto centre = getStatePositionCentre();
            auto control = mid + (mid - centre) * 0.18f;

            juce::Path curve;
            curve.startNewSubPath (from);
            curve.quadraticTo (control, to);

            g.setColour (accentB().withAlpha (0.22f + juce::jlimit (0.0f, 0.4f, rule.weight * 0.08f)));
            g.strokePath (curve, juce::PathStrokeType (2.0f + rule.weight));

            auto arrowPoint = from + (to - from) * 0.78f;
            g.setColour (accentB().withAlpha (0.72f));
            g.fillEllipse (arrowPoint.x - 3.0f, arrowPoint.y - 3.0f, 6.0f, 6.0f);
        }
    }

    juce::Point<float> getStatePositionCentre() const
    {
        if (statePositions.empty())
            return getLocalBounds().toFloat().getCentre();

        juce::Point<float> centre;
        for (const auto& position : statePositions)
            centre += position;

        return centre / static_cast<float> (statePositions.size());
    }

    void drawStates (juce::Graphics& g)
    {
        for (int i = 0; i < machine->getStateCount(); ++i)
        {
            auto p = statePositions[static_cast<size_t> (i)];
            auto selected = i == machine->selectedState;
            auto laneCount = machine->getLaneCount (i);

            juce::ColourGradient glow (selected ? accentA().withAlpha (0.55f) : accentC().withAlpha (0.18f),
                                       p.translated (-stateRadius, -stateRadius),
                                       juce::Colours::transparentBlack,
                                       p.translated (stateRadius, stateRadius),
                                       true);
            g.setGradientFill (glow);
            g.fillEllipse (p.x - stateRadius * 1.55f, p.y - stateRadius * 1.55f, stateRadius * 3.1f, stateRadius * 3.1f);

            g.setColour (selected ? selectedFill() : juce::Colour (0xff252a31));
            g.fillEllipse (p.x - stateRadius, p.y - stateRadius, stateRadius * 2.0f, stateRadius * 2.0f);

            g.setColour ((selected ? accentA() : mutedInk()).withAlpha (0.95f));
            g.drawEllipse (p.x - stateRadius, p.y - stateRadius, stateRadius * 2.0f, stateRadius * 2.0f, selected ? 3.0f : 1.5f);

            const auto* child = machine->childMachine (i);
            if (child != nullptr)
            {
                auto nestedRadius = stateRadius + 7.0f;
                g.setColour ((selected ? accentC() : accentB()).withAlpha (selected ? 0.95f : 0.62f));
                g.drawEllipse (p.x - nestedRadius, p.y - nestedRadius, nestedRadius * 2.0f, nestedRadius * 2.0f, 2.0f);
                drawNestedIndicator (g, *child, p, selected, child == inspectedMachine);
            }

            g.setColour (ink());
            g.setFont (juce::FontOptions (stateRadius < 40.0f ? 13.0f : 16.0f, juce::Font::bold));
            g.drawFittedText (machine->state (i).name, juce::Rectangle<int> (static_cast<int> (p.x - stateRadius * 0.95f),
                                                                            static_cast<int> (p.y - stateRadius * 0.38f),
                                                                            static_cast<int> (stateRadius * 1.9f), 22),
                              juce::Justification::centred, 1);

            g.setColour (mutedInk());
            g.setFont (juce::FontOptions (stateRadius < 40.0f ? 10.0f : 12.0f));
            g.drawFittedText (juce::String (laneCount) + (laneCount == 1 ? " lane" : " lanes"),
                              juce::Rectangle<int> (static_cast<int> (p.x - stateRadius * 0.9f),
                                                    static_cast<int> (p.y + stateRadius * 0.10f),
                                                    static_cast<int> (stateRadius * 1.8f), 18),
                              juce::Justification::centred, 1);
        }
    }

    void drawNestedIndicator (juce::Graphics& g, const MachineModel& child, juce::Point<float> parentCentre, bool parentSelected, bool childInspected)
    {
        const auto childCount = child.getStateCount();
        if (childCount <= 0)
            return;

        const auto orbitRadius = getNestedOrbitRadius();
        const auto nodeRadius = getNestedNodeRadius (childCount);
        std::vector<juce::Point<float>> childPoints;
        childPoints.reserve (static_cast<size_t> (childCount));

        for (int j = 0; j < childCount; ++j)
        {
            const auto angle = (juce::MathConstants<float>::twoPi * static_cast<float> (j) / static_cast<float> (childCount))
                             - juce::MathConstants<float>::halfPi;
            childPoints.push_back ({ parentCentre.x + std::cos (angle) * orbitRadius,
                                     parentCentre.y + std::sin (angle) * orbitRadius });
        }

        g.setColour ((childInspected ? inspectedFill() : juce::Colour (0xff11161d)).withAlpha (childInspected ? 0.88f : 0.78f));
        g.fillEllipse (parentCentre.x - orbitRadius - 4.0f, parentCentre.y - orbitRadius - 4.0f,
                       (orbitRadius + 4.0f) * 2.0f, (orbitRadius + 4.0f) * 2.0f);

        const auto ringColour = childInspected ? accentB() : juce::Colour (0xff6f7b88);
        g.setColour (ringColour.withAlpha (childInspected ? 0.9f : (parentSelected ? 0.58f : 0.38f)));
        g.drawEllipse (parentCentre.x - orbitRadius, parentCentre.y - orbitRadius,
                       orbitRadius * 2.0f, orbitRadius * 2.0f, childInspected ? 2.4f : (parentSelected ? 2.0f : 1.3f));

        for (const auto& rule : child.rules)
        {
            if (rule.from < 0 || rule.to < 0 || rule.from >= childCount || rule.to >= childCount)
                continue;

            auto from = childPoints[static_cast<size_t> (rule.from)];
            auto to = childPoints[static_cast<size_t> (rule.to)];
            auto mid = (from + to) * 0.5f;
            auto control = mid + ((mid - parentCentre) * 0.14f);
            juce::Path path;
            path.startNewSubPath (from);
            path.quadraticTo (control, to);
            g.setColour ((parentSelected ? accentA() : accentB()).withAlpha (0.18f));
            g.strokePath (path, juce::PathStrokeType (1.0f));
        }

        for (int j = 0; j < childCount; ++j)
        {
            const auto point = childPoints[static_cast<size_t> (j)];
            const auto selected = j == child.selectedState;
            const auto stateColour = paletteColour (j);
            g.setColour ((selected && childInspected ? stateColour.brighter (0.35f) : stateColour).withAlpha (selected ? 0.98f : 0.84f));
            g.fillEllipse (point.x - nodeRadius, point.y - nodeRadius, nodeRadius * 2.0f, nodeRadius * 2.0f);
            g.setColour ((selected ? ink() : juce::Colour (0xff101318)).withAlpha (selected ? 0.82f : 0.92f));
            g.drawEllipse (point.x - nodeRadius, point.y - nodeRadius, nodeRadius * 2.0f, nodeRadius * 2.0f, selected ? 1.4f : 1.0f);

            if (auto* grandchild = child.childMachine (j))
                drawSecondLayerIndicator (g, *grandchild, point, selected && childInspected);
        }

        auto badge = getNestedBadgeBounds (child, parentCentre);
        g.setColour (juce::Colour (0xff101318).withAlpha (0.96f));
        g.fillRoundedRectangle (badge, 6.0f);
        g.setColour ((childInspected ? accentB() : (parentSelected ? accentA() : accentC())).withAlpha (0.88f));
        g.drawRoundedRectangle (badge, 6.0f, 1.1f);
        g.setColour (ink());
        g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
        g.drawFittedText (juce::String (childCount), badge.toNearestInt(), juce::Justification::centred, 1);
    }

    void drawSecondLayerIndicator (juce::Graphics& g, const MachineModel& grandchild, juce::Point<float> childStateCentre, bool selected)
    {
        const auto count = grandchild.getStateCount();
        if (count <= 0)
            return;

        const auto orbit = getSecondLayerOrbitRadius();
        const auto nodeRadius = getSecondLayerNodeRadius (count);

        g.setColour ((selected ? accentA() : accentC()).withAlpha (selected ? 0.70f : 0.42f));
        g.drawEllipse (childStateCentre.x - orbit, childStateCentre.y - orbit, orbit * 2.0f, orbit * 2.0f, selected ? 1.35f : 0.9f);

        for (int k = 0; k < count; ++k)
        {
            const auto angle = (juce::MathConstants<float>::twoPi * static_cast<float> (k) / static_cast<float> (count))
                             - juce::MathConstants<float>::halfPi;
            const auto point = juce::Point<float> { childStateCentre.x + std::cos (angle) * orbit,
                                                    childStateCentre.y + std::sin (angle) * orbit };
            const auto stateSelected = k == grandchild.selectedState;
            g.setColour (paletteColour (k + 3).withAlpha (stateSelected ? 0.95f : 0.70f));
            g.fillEllipse (point.x - nodeRadius, point.y - nodeRadius, nodeRadius * 2.0f, nodeRadius * 2.0f);
        }

        auto badge = getSecondLayerBadgeBounds (grandchild, childStateCentre);
        g.setColour (juce::Colour (0xff101318).withAlpha (0.94f));
        g.fillRoundedRectangle (badge, 4.5f);
        g.setColour ((selected ? accentA() : accentC()).withAlpha (0.82f));
        g.drawRoundedRectangle (badge, 4.5f, 0.9f);
        g.setColour (ink().withAlpha (0.95f));
        g.setFont (juce::FontOptions (8.8f, juce::Font::bold));
        g.drawFittedText (juce::String (count), badge.toNearestInt(), juce::Justification::centred, 1);
    }

    int hitTestNestedState (const MachineModel& child, juce::Point<float> parentCentre, juce::Point<float> pointer) const
    {
        const auto childCount = child.getStateCount();
        if (childCount <= 0)
            return -1;

        const auto orbitRadius = getNestedOrbitRadius();
        const auto hitRadius = juce::jmax (7.0f, getNestedNodeRadius (childCount) + 5.0f);
        for (int j = 0; j < childCount; ++j)
        {
            const auto angle = (juce::MathConstants<float>::twoPi * static_cast<float> (j) / static_cast<float> (childCount))
                             - juce::MathConstants<float>::halfPi;
            juce::Point<float> point { parentCentre.x + std::cos (angle) * orbitRadius,
                                       parentCentre.y + std::sin (angle) * orbitRadius };

            if (point.getDistanceFrom (pointer) <= hitRadius)
                return j;
        }

        return -1;
    }

    int hitTestSecondLayerBadge (const MachineModel& child, juce::Point<float> parentCentre, juce::Point<float> pointer) const
    {
        for (int j = 0; j < child.getStateCount(); ++j)
        {
            auto* grandchild = child.childMachine (j);
            if (grandchild == nullptr)
                continue;

            if (getSecondLayerBadgeBounds (*grandchild, getNestedStatePoint (child, parentCentre, j)).contains (pointer))
                return j;
        }

        return -1;
    }

    std::pair<int, int> hitTestSecondLayerNestedState (const MachineModel& child, juce::Point<float> parentCentre, juce::Point<float> pointer) const
    {
        const auto childCount = child.getStateCount();
        const auto firstOrbit = getNestedOrbitRadius();
        const auto hitRadius = juce::jmax (5.5f, getSecondLayerNodeRadius (maxStateCount) + 4.0f);

        for (int j = 0; j < childCount; ++j)
        {
            auto* grandchild = child.childMachine (j);
            if (grandchild == nullptr)
                continue;

            const auto childAngle = (juce::MathConstants<float>::twoPi * static_cast<float> (j) / static_cast<float> (childCount))
                                  - juce::MathConstants<float>::halfPi;
            const auto childPoint = juce::Point<float> { parentCentre.x + std::cos (childAngle) * firstOrbit,
                                                         parentCentre.y + std::sin (childAngle) * firstOrbit };

            const auto secondOrbit = getSecondLayerOrbitRadius();
            for (int k = 0; k < grandchild->getStateCount(); ++k)
            {
                const auto angle = (juce::MathConstants<float>::twoPi * static_cast<float> (k) / static_cast<float> (grandchild->getStateCount()))
                                 - juce::MathConstants<float>::halfPi;
                const auto point = juce::Point<float> { childPoint.x + std::cos (angle) * secondOrbit,
                                                        childPoint.y + std::sin (angle) * secondOrbit };

                if (point.getDistanceFrom (pointer) <= hitRadius)
                    return { j, k };
            }
        }

        return { -1, -1 };
    }

    juce::Point<float> getNestedStatePoint (const MachineModel& child, juce::Point<float> parentCentre, int stateIndex) const
    {
        const auto childCount = juce::jmax (1, child.getStateCount());
        const auto angle = (juce::MathConstants<float>::twoPi * static_cast<float> (stateIndex) / static_cast<float> (childCount))
                         - juce::MathConstants<float>::halfPi;
        const auto orbitRadius = getNestedOrbitRadius();
        return { parentCentre.x + std::cos (angle) * orbitRadius,
                 parentCentre.y + std::sin (angle) * orbitRadius };
    }

    juce::Rectangle<float> getNestedBadgeBounds (const MachineModel& child, juce::Point<float> parentCentre) const
    {
        juce::ignoreUnused (child);
        const auto badgeWidth = stateRadius < 40.0f ? 24.0f : 28.0f;
        const auto badgeHeight = stateRadius < 40.0f ? 18.0f : 20.0f;
        const auto badgeRadius = getNestedOrbitRadius() + badgeWidth * 0.66f + 7.0f;
        const auto angle = juce::MathConstants<float>::pi * 0.25f;
        const auto centre = juce::Point<float> { parentCentre.x + std::cos (angle) * badgeRadius,
                                                 parentCentre.y + std::sin (angle) * badgeRadius };
        return juce::Rectangle<float> (0.0f, 0.0f, badgeWidth, badgeHeight).withCentre (centre);
    }

    juce::Rectangle<float> getSecondLayerBadgeBounds (const MachineModel& grandchild, juce::Point<float> childStateCentre) const
    {
        juce::ignoreUnused (grandchild);
        const auto scale = juce::jlimit (0.9f, 1.8f, stateRadius / 54.0f);
        const auto badgeWidth = 20.0f * scale;
        const auto badgeHeight = 15.0f * scale;
        const auto badgeRadius = getSecondLayerOrbitRadius() + badgeWidth * 0.68f + 4.0f;
        const auto angle = juce::MathConstants<float>::pi * 0.25f;
        const auto centre = juce::Point<float> { childStateCentre.x + std::cos (angle) * badgeRadius,
                                                 childStateCentre.y + std::sin (angle) * badgeRadius };
        return juce::Rectangle<float> (0.0f, 0.0f, badgeWidth, badgeHeight).withCentre (centre);
    }

    float getOuterNodeExtent() const
    {
        auto extent = stateRadius * 1.55f;

        for (int i = 0; i < machine->getStateCount(); ++i)
            extent = juce::jmax (extent, getNodeVisualExtent (i));

        return extent;
    }

    float getNodeClearance (int stateIndex) const
    {
        return getNodeVisualExtent (stateIndex) + 22.0f;
    }

    float getNodeVisualExtent (int stateIndex) const
    {
        auto extent = stateRadius * 1.55f;
        const auto* child = machine->childMachine (stateIndex);

        if (child == nullptr)
            return extent;

        extent = getNestedOrbitRadius() + getNestedNodeRadius (child->getStateCount()) + 10.0f;

        if (childHasGrandchildren (*child))
            extent += getSecondLayerOrbitRadius() + getSecondLayerBadgeOuterExtent() + 18.0f;
        else
            extent += getNestedBadgeOuterExtent() + 8.0f;

        return extent;
    }

    bool childHasGrandchildren (const MachineModel& child) const
    {
        for (int i = 0; i < child.getStateCount(); ++i)
            if (child.childMachine (i) != nullptr)
                return true;

        return false;
    }

    float getNestedOrbitRadius() const
    {
        return stateRadius + juce::jmap (stateRadius, 34.0f, 54.0f, 10.0f, 16.0f);
    }

    float getNestedNodeRadius (int childCount) const
    {
        return childCount > 7 || stateRadius < 40.0f ? 2.5f : 3.7f;
    }

    float getNestedBadgeOuterExtent() const
    {
        const auto badgeWidth = stateRadius < 40.0f ? 24.0f : 28.0f;
        return badgeWidth * 1.45f;
    }

    float getSecondLayerBadgeOuterExtent() const
    {
        const auto scale = juce::jlimit (0.9f, 1.8f, stateRadius / 54.0f);
        return 20.0f * scale * 1.55f;
    }

    float getSecondLayerOrbitRadius() const
    {
        return juce::jmax (18.0f, stateRadius * 0.63f);
    }

    float getSecondLayerNodeRadius (int childCount) const
    {
        const auto scale = juce::jlimit (1.15f, 3.2f, stateRadius / 42.0f);
        return (childCount > 7 ? 3.3f : 4.5f) * scale;
    }

    void startNestedStateCountEdit (int parentStateIndex, juce::Rectangle<float> badgeBounds)
    {
        auto* child = machine->childMachine (parentStateIndex);
        if (child == nullptr)
            return;

        finishNestedStateCountEdit (false);
        editingNestedParentState = parentStateIndex;
        auto safeThis = juce::Component::SafePointer<GraphComponent> (this);

        nestedCountEditor = std::make_unique<juce::TextEditor>();
        nestedCountEditor->setText (juce::String (child->getStateCount()), false);
        nestedCountEditor->setSelectAllWhenFocused (true);
        nestedCountEditor->setInputRestrictions (2, "0123456789");
        nestedCountEditor->setJustification (juce::Justification::centred);
        nestedCountEditor->setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff101318));
        nestedCountEditor->setColour (juce::TextEditor::textColourId, ink());
        nestedCountEditor->setColour (juce::TextEditor::highlightColourId, accentA().withAlpha (0.35f));
        nestedCountEditor->setColour (juce::TextEditor::outlineColourId, accentA());
        nestedCountEditor->onReturnKey = [safeThis] { if (safeThis != nullptr) safeThis->finishNestedStateCountEdit (true); };
        nestedCountEditor->onEscapeKey = [safeThis] { if (safeThis != nullptr) safeThis->finishNestedStateCountEdit (false); };
        nestedCountEditor->onFocusLost = [safeThis] { if (safeThis != nullptr) safeThis->finishNestedStateCountEdit (true); };

        addAndMakeVisible (*nestedCountEditor);
        nestedCountEditor->setBounds (badgeBounds.expanded (4.0f, 3.0f).toNearestInt());
        nestedCountEditor->grabKeyboardFocus();
        nestedCountEditor->selectAll();
    }

    void startSecondLayerStateCountEdit (int parentStateIndex, int childStateIndex, juce::Rectangle<float> badgeBounds)
    {
        auto* child = machine->childMachine (parentStateIndex);
        auto* grandchild = child != nullptr ? child->childMachine (childStateIndex) : nullptr;
        if (grandchild == nullptr)
            return;

        finishNestedStateCountEdit (false);
        editingNestedParentState = parentStateIndex;
        editingSecondLayerChildState = childStateIndex;
        auto safeThis = juce::Component::SafePointer<GraphComponent> (this);

        nestedCountEditor = std::make_unique<juce::TextEditor>();
        nestedCountEditor->setText (juce::String (grandchild->getStateCount()), false);
        nestedCountEditor->setSelectAllWhenFocused (true);
        nestedCountEditor->setInputRestrictions (2, "0123456789");
        nestedCountEditor->setJustification (juce::Justification::centred);
        nestedCountEditor->setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff101318));
        nestedCountEditor->setColour (juce::TextEditor::textColourId, ink());
        nestedCountEditor->setColour (juce::TextEditor::highlightColourId, accentC().withAlpha (0.35f));
        nestedCountEditor->setColour (juce::TextEditor::outlineColourId, accentC());
        nestedCountEditor->onReturnKey = [safeThis] { if (safeThis != nullptr) safeThis->finishNestedStateCountEdit (true); };
        nestedCountEditor->onEscapeKey = [safeThis] { if (safeThis != nullptr) safeThis->finishNestedStateCountEdit (false); };
        nestedCountEditor->onFocusLost = [safeThis] { if (safeThis != nullptr) safeThis->finishNestedStateCountEdit (true); };

        addAndMakeVisible (*nestedCountEditor);
        nestedCountEditor->setBounds (badgeBounds.expanded (4.0f, 3.0f).toNearestInt());
        nestedCountEditor->grabKeyboardFocus();
        nestedCountEditor->selectAll();
    }

    void finishNestedStateCountEdit (bool commit)
    {
        if (nestedCountEditor == nullptr)
            return;

        const auto parentStateIndex = editingNestedParentState;
        const auto text = nestedCountEditor->getText();

        auto editor = std::move (nestedCountEditor);
        const auto childStateIndex = editingSecondLayerChildState;
        editingNestedParentState = -1;
        editingSecondLayerChildState = -1;
        removeChildComponent (editor.get());
        editor.reset();

        if (! commit || parentStateIndex < 0)
            return;

        const auto newCount = juce::jlimit (1, maxStateCount, text.getIntValue());
        if (childStateIndex >= 0)
        {
            if (onSecondLayerNestedStateCountChanged)
                onSecondLayerNestedStateCountChanged (parentStateIndex, childStateIndex, newCount);
        }
        else if (onNestedStateCountChanged)
            onNestedStateCountChanged (parentStateIndex, newCount);
    }

    void timerCallback() override { repaint(); }

    MachineModel* machine;
    MachineModel* inspectedMachine = nullptr;
    std::vector<juce::Point<float>> statePositions;
    std::vector<juce::Point<float>> lockedScreenPositions;
    std::unique_ptr<juce::TextEditor> nestedCountEditor;
    float stateRadius = 48.0f;
    float lockedScreenRadius = 48.0f;
    float zoom = 1.0f;
    juce::Point<float> panOffset;
    juce::Point<float> dragStart;
    juce::Point<float> panStart;
    std::vector<juce::Point<float>> manualNodeOffsets;
    juce::Point<float> nodeOffsetStart;
    int draggingStateIndex = -1;
    bool draggedState = false;
    bool nodePositionLockActive = false;
    int editingNestedParentState = -1;
    int editingSecondLayerChildState = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GraphComponent)
};

class RuleListComponent final : public juce::Component
{
public:
    explicit RuleListComponent (MachineModel& modelToUse) : machine (&modelToUse)
    {
        addAndMakeVisible (fromBox);
        addAndMakeVisible (toBox);
        addAndMakeVisible (weightSlider);
        addAndMakeVisible (addButton);
        addAndMakeVisible (updateButton);
        addAndMakeVisible (removeButton);
        addAndMakeVisible (ringButton);

        weightSlider.setRange (0.1, 5.0, 0.1);
        weightSlider.setValue (1.0);
        weightSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 56, 22);

        addButton.setButtonText ("Add");
        updateButton.setButtonText ("Save");
        removeButton.setButtonText ("Delete");
        ringButton.setButtonText ("Rules");

        addButton.onClick = [this]
        {
            addRuleFromControls();
        };

        updateButton.onClick = [this]
        {
            updateSelectedRule();
        };

        removeButton.onClick = [this]
        {
            removeSelectedRule();
        };

        ringButton.onClick = [this]
        {
            machine->regenerateRingRules();
            selectedRuleIndex = -1;
            refreshChoices();
            if (onRulesChanged)
                onRulesChanged();
        };

        refreshChoices();
    }

    void setMachine (MachineModel& modelToUse)
    {
        machine = &modelToUse;
        selectedRuleIndex = -1;
        refreshChoices();
        repaint();
    }

    std::function<void()> onRulesChanged;

    void refreshChoices()
    {
        fromBox.clear();
        toBox.clear();
        for (int i = 0; i < machine->getStateCount(); ++i)
        {
            fromBox.addItem (machine->state (i).name, i + 1);
            toBox.addItem (machine->state (i).name, i + 1);
        }
        selectedRuleIndex = selectedRuleIndex >= static_cast<int> (machine->rules.size()) ? -1 : selectedRuleIndex;

        if (selectedRuleIndex >= 0)
            loadRuleIntoControls (selectedRuleIndex);
        else
        {
            fromBox.setSelectedItemIndex (machine->selectedState);
            toBox.setSelectedItemIndex ((machine->selectedState + 1) % machine->getStateCount());
            weightSlider.setValue (1.0, juce::dontSendNotification);
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff181b20));
        g.setColour (ink());
        g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        g.drawText ("Transition rules", getLocalBounds().removeFromTop (28), juce::Justification::centredLeft);

        auto list = getRuleListBounds();
        g.setFont (juce::FontOptions (12.5f));

        for (int i = 0; i < static_cast<int> (machine->rules.size()); ++i)
        {
            auto row = list.removeFromTop (26);
            const auto& r = machine->rules[static_cast<size_t> (i)];
            const auto selected = i == selectedRuleIndex;
            g.setColour (selected ? selectedRow() : (i % 2 == 0 ? juce::Colour (0xff20252c) : juce::Colour (0xff1b2026)));
            g.fillRoundedRectangle (row.toFloat().reduced (1.0f), 4.0f);
            if (selected)
            {
                g.setColour (accentC().withAlpha (0.88f));
                g.fillRoundedRectangle (row.removeFromLeft (4).toFloat().reduced (0.0f, 4.0f), 2.0f);
            }

            g.setColour (selected ? ink() : mutedInk());

            auto rowArea = row.reduced (8, 0);
            g.drawText (machine->state (r.from).name, rowArea.removeFromLeft (96), juce::Justification::centredLeft);
            g.drawText ("->", rowArea.removeFromLeft (24), juce::Justification::centred);
            g.drawText (machine->state (r.to).name, rowArea.removeFromLeft (96), juce::Justification::centredLeft);
            g.drawText ("w " + juce::String (r.weight, 1), rowArea.removeFromRight (52), juce::Justification::centredRight);
        }
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        auto list = getRuleListBounds();
        for (int i = 0; i < static_cast<int> (machine->rules.size()); ++i)
        {
            auto row = list.removeFromTop (26);
            if (row.contains (event.getPosition()))
            {
                selectedRuleIndex = i;
                loadRuleIntoControls (i);
                repaint();
                return;
            }
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (0, 30).removeFromTop (36);
        fromBox.setBounds (area.removeFromLeft (82).reduced (0, 4));
        toBox.setBounds (area.removeFromLeft (82).reduced (4));
        weightSlider.setBounds (area.removeFromLeft (96).reduced (4));
        addButton.setBounds (area.removeFromLeft (54).reduced (4));
        updateButton.setBounds (area.removeFromLeft (58).reduced (4));
        removeButton.setBounds (area.removeFromLeft (62).reduced (4));
        ringButton.setBounds (area.removeFromLeft (64).reduced (4));
    }

private:
    juce::Rectangle<int> getRuleListBounds() const
    {
        return getLocalBounds().withTrimmedTop (76).reduced (0, 6);
    }

    void addRuleFromControls()
    {
        auto from = fromBox.getSelectedItemIndex();
        auto to = toBox.getSelectedItemIndex();
        if (from < 0 || to < 0)
            return;

        machine->rules.push_back ({ from, to, static_cast<float> (weightSlider.getValue()) });
        selectedRuleIndex = static_cast<int> (machine->rules.size()) - 1;
        if (onRulesChanged)
            onRulesChanged();
        repaint();
    }

    void updateSelectedRule()
    {
        if (selectedRuleIndex < 0 || selectedRuleIndex >= static_cast<int> (machine->rules.size()))
            return;

        auto from = fromBox.getSelectedItemIndex();
        auto to = toBox.getSelectedItemIndex();
        if (from < 0 || to < 0)
            return;

        machine->rules[static_cast<size_t> (selectedRuleIndex)] = { from, to, static_cast<float> (weightSlider.getValue()) };
        if (onRulesChanged)
            onRulesChanged();
        repaint();
    }

    void removeSelectedRule()
    {
        if (selectedRuleIndex < 0 || selectedRuleIndex >= static_cast<int> (machine->rules.size()))
            return;

        machine->rules.erase (machine->rules.begin() + selectedRuleIndex);
        selectedRuleIndex = juce::jmin (selectedRuleIndex, static_cast<int> (machine->rules.size()) - 1);
        refreshChoices();
        if (onRulesChanged)
            onRulesChanged();
        repaint();
    }

    void loadRuleIntoControls (int index)
    {
        if (index < 0 || index >= static_cast<int> (machine->rules.size()))
            return;

        const auto& rule = machine->rules[static_cast<size_t> (index)];
        fromBox.setSelectedItemIndex (rule.from, juce::dontSendNotification);
        toBox.setSelectedItemIndex (rule.to, juce::dontSendNotification);
        weightSlider.setValue (rule.weight, juce::dontSendNotification);
    }

    MachineModel* machine;
    juce::ComboBox fromBox;
    juce::ComboBox toBox;
    juce::Slider weightSlider;
    juce::TextButton addButton;
    juce::TextButton updateButton;
    juce::TextButton removeButton;
    juce::TextButton ringButton;
    int selectedRuleIndex = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RuleListComponent)
};

class PillBar final : public juce::Component
{
public:
    std::function<void (int)> onIndexSelected;

    void setItems (const juce::StringArray& names, int selected)
    {
        buttons.clear();
        selectedIndex = selected;

        for (int i = 0; i < names.size(); ++i)
        {
            auto button = std::make_unique<juce::TextButton> (names[i]);
            button->setClickingTogglesState (false);
            button->onClick = [this, i]
            {
                selectedIndex = i;
                if (onIndexSelected)
                    onIndexSelected (i);
                repaint();
            };
            addAndMakeVisible (*button);
            buttons.push_back (std::move (button));
        }

        resized();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (juce::Colour (0xff181b20));
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 7.0f);
    }

    void resized() override
    {
        if (buttons.empty())
            return;

        auto area = getLocalBounds().reduced (3);
        auto width = area.getWidth() / static_cast<int> (buttons.size());

        for (int i = 0; i < static_cast<int> (buttons.size()); ++i)
        {
            auto cell = area.removeFromLeft (i == static_cast<int> (buttons.size()) - 1 ? area.getWidth() : width);
            buttons[static_cast<size_t> (i)]->setBounds (cell.reduced (2, 1));
            buttons[static_cast<size_t> (i)]->setColour (juce::TextButton::buttonColourId,
                                                         i == selectedIndex ? selectedFill() : juce::Colour (0xff252a31));
            buttons[static_cast<size_t> (i)]->setColour (juce::TextButton::buttonOnColourId, selectedFill());
            buttons[static_cast<size_t> (i)]->setColour (juce::TextButton::textColourOffId,
                                                         i == selectedIndex ? accentA().brighter (0.2f) : mutedInk());
        }
    }

private:
    std::vector<std::unique_ptr<juce::TextButton>> buttons;
    int selectedIndex = 0;
};

class ClickableLabel final : public juce::Label
{
public:
    std::function<void()> onClick;

    void mouseUp (const juce::MouseEvent&) override
    {
        if (onClick)
            onClick();
    }
};

class TrackListComponent final : public juce::Component
{
public:
    std::function<void (int)> onTrackSelected;
    std::function<void (int)> onEnabledToggled;
    std::function<void (int)> onMuteToggled;
    std::function<void (int)> onSoloToggled;
    std::function<void (int, float)> onVolumeChanged;

    void setState (State& stateToShow, int selectedLane)
    {
        state = &stateToShow;
        selectedIndex = selectedLane;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds();
        g.setColour (juce::Colour (0xff181b20));
        g.fillRoundedRectangle (bounds.toFloat(), 7.0f);

        if (state == nullptr)
            return;

        auto list = getTrackListBounds();
        for (int i = 0; i < static_cast<int> (state->lanes.size()); ++i)
        {
            auto row = list.removeFromTop (43).reduced (0, 4);
            const auto& lane = state->lanes[static_cast<size_t> (i)];
            const auto selected = i == selectedIndex;

            const auto laneColour = getTrackColour (i);
            g.setColour (selected ? selectedRow() : juce::Colour (0xff20252c).withAlpha (lane.enabled ? 1.0f : 0.48f));
            g.fillRoundedRectangle (row.toFloat(), 5.0f);
            if (selected)
            {
                g.setColour (laneColour.withAlpha (0.45f));
                g.fillRoundedRectangle (row.withWidth (5).toFloat(), 3.0f);
            }

            g.setColour (selected ? laneColour.withAlpha (0.95f) : juce::Colour (0xff414a55));
            g.drawRoundedRectangle (row.toFloat(), 5.0f, selected ? 1.4f : 0.8f);

            auto rowText = row.reduced (10, 0);
            auto dotArea = rowText.removeFromLeft (12).withSizeKeepingCentre (8, 8).toFloat();
            g.setColour (laneColour.withAlpha (lane.enabled ? (lane.playing ? 0.96f : 0.72f) : 0.28f));
            g.fillEllipse (dotArea);
            g.setColour (lane.playing ? ink().withAlpha (0.85f) : juce::Colour (0xff101318).withAlpha (0.8f));
            g.drawEllipse (dotArea.expanded (1.0f), lane.playing ? 1.4f : 0.8f);

            auto buttons = rowText.removeFromRight (82);
            drawToggle (g, buttons.removeFromLeft (25), "E", lane.enabled, accentB());
            drawToggle (g, buttons.removeFromLeft (25), "M", lane.muted, accentC());
            drawToggle (g, buttons.removeFromLeft (25), "S", lane.solo, accentA());

            auto volumeArea = rowText.removeFromRight (68).reduced (8, 0);
            drawVolumeControl (g, volumeArea, lane.volume, laneColour, lane.enabled);

            g.setColour (selected ? ink() : mutedInk().withAlpha (lane.enabled ? 1.0f : 0.52f));
            g.setFont (juce::FontOptions (12.5f, selected ? juce::Font::bold : juce::Font::plain));
            g.drawFittedText (lane.name, rowText, juce::Justification::centredLeft, 1);
        }
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        if (state == nullptr)
            return;

        auto list = getTrackListBounds();
        for (int i = 0; i < static_cast<int> (state->lanes.size()); ++i)
        {
            auto row = list.removeFromTop (43).reduced (0, 4);
            if (row.contains (event.getPosition()))
            {
                auto controls = row.reduced (10, 0).removeFromRight (82);
                auto enabledArea = controls.removeFromLeft (25);
                auto muteArea = controls.removeFromLeft (25);
                auto soloArea = controls.removeFromLeft (25);
                auto volumeArea = getVolumeBoundsForRow (row);
                selectedIndex = i;
                if (volumeArea.contains (event.getPosition()))
                {
                    draggingVolumeIndex = i;
                    updateVolumeFromMouse (i, event.position.x);
                }
                else if (enabledArea.contains (event.getPosition()))
                {
                    if (onEnabledToggled)
                        onEnabledToggled (i);
                }
                else if (muteArea.contains (event.getPosition()))
                {
                    if (onMuteToggled)
                        onMuteToggled (i);
                }
                else if (soloArea.contains (event.getPosition()))
                {
                    if (onSoloToggled)
                        onSoloToggled (i);
                }
                else if (onTrackSelected)
                    onTrackSelected (i);
                repaint();
                return;
            }
        }
    }

    void mouseDrag (const juce::MouseEvent& event) override
    {
        if (state == nullptr || draggingVolumeIndex < 0)
            return;

        updateVolumeFromMouse (draggingVolumeIndex, event.position.x);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        draggingVolumeIndex = -1;
    }

private:
    juce::Colour getTrackColour (int index) const
    {
        return paletteColour (index);
    }

    void drawToggle (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& text, bool active, juce::Colour colour) const
    {
        auto pill = area.reduced (2, 9).toFloat();
        g.setColour (active ? colour.withAlpha (0.92f) : juce::Colour (0xff111318));
        g.fillRoundedRectangle (pill, 3.0f);
        g.setColour (active ? juce::Colour (0xff111318).withAlpha (0.78f) : juce::Colour (0xff4b5560));
        g.drawRoundedRectangle (pill, 3.0f, active ? 0.8f : 1.0f);
        g.setColour (active ? juce::Colour (0xff101318) : mutedInk().withAlpha (0.72f));
        g.setFont (juce::FontOptions (9.5f, juce::Font::bold));
        g.drawText (text, area, juce::Justification::centred);
    }

    void drawVolumeControl (juce::Graphics& g, juce::Rectangle<int> area, float volume, juce::Colour colour, bool enabled) const
    {
        const auto clipped = juce::jlimit (0.0f, 1.0f, volume);
        auto valueArea = area.removeFromRight (24);
        auto slider = area.reduced (0, 12);

        g.setColour (juce::Colour (0xff111318).withAlpha (enabled ? 1.0f : 0.55f));
        g.fillRoundedRectangle (slider.toFloat(), 2.0f);

        auto fill = slider.toFloat();
        fill.setWidth (juce::jmax (2.0f, fill.getWidth() * clipped));
        g.setColour (colour.withAlpha (enabled ? 0.78f : 0.24f));
        g.fillRoundedRectangle (fill, 2.0f);

        g.setColour (mutedInk().withAlpha (enabled ? 0.72f : 0.38f));
        g.setFont (juce::FontOptions (9.5f, juce::Font::bold));
        g.drawText (juce::String (clipped, 2), valueArea, juce::Justification::centredRight);
    }

    juce::Rectangle<int> getVolumeBoundsForRow (juce::Rectangle<int> row) const
    {
        auto rowText = row.reduced (10, 0);
        rowText.removeFromLeft (12);
        rowText.removeFromRight (82);
        return rowText.removeFromRight (68).reduced (8, 7);
    }

    void updateVolumeFromMouse (int index, float x)
    {
        if (state == nullptr || index < 0 || index >= static_cast<int> (state->lanes.size()))
            return;

        auto list = getTrackListBounds();
        auto row = list.removeFromTop (43 * index + 43).removeFromBottom (43).reduced (0, 4);
        auto volumeArea = getVolumeBoundsForRow (row);
        const auto newVolume = juce::jlimit (0.0f, 1.0f, (x - static_cast<float> (volumeArea.getX())) / static_cast<float> (juce::jmax (1, volumeArea.getWidth() - 24)));

        if (onVolumeChanged)
            onVolumeChanged (index, newVolume);

        repaint();
    }

    juce::Rectangle<int> getTrackListBounds() const
    {
        return getLocalBounds().reduced (8, 8);
    }

    State* state = nullptr;
    int selectedIndex = 0;
    int draggingVolumeIndex = -1;
};

class PaneDivider final : public juce::Component
{
public:
    enum class Orientation
    {
        vertical,
        horizontal
    };

    std::function<void()> onDragStarted;
    std::function<void (int)> onDragged;
    std::function<void()> onDragEnded;

    explicit PaneDivider (Orientation orientationToUse = Orientation::vertical) : orientation (orientationToUse)
    {
        setMouseCursor (orientation == Orientation::vertical ? juce::MouseCursor::LeftRightResizeCursor
                                                             : juce::MouseCursor::UpDownResizeCursor);
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();
        g.setColour (juce::Colour (0xff0f1116).withAlpha (0.72f));
        g.fillRoundedRectangle (orientation == Orientation::vertical ? bounds.reduced (2.0f, 0.0f)
                                                                     : bounds.reduced (0.0f, 2.0f), 3.0f);
        g.setColour (accentB().withAlpha (isMouseOverOrDragging() ? 0.62f : 0.18f));
        g.fillRoundedRectangle (orientation == Orientation::vertical
                                    ? bounds.withSizeKeepingCentre (1.5f, bounds.getHeight() - 20.0f)
                                    : bounds.withSizeKeepingCentre (bounds.getWidth() - 28.0f, 1.5f), 1.0f);
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        if (onDragStarted)
            onDragStarted();
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& event) override
    {
        if (onDragged)
            onDragged (orientation == Orientation::vertical ? event.getDistanceFromDragStartX()
                                                            : event.getDistanceFromDragStartY());
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (onDragEnded)
            onDragEnded();

        repaint();
    }

private:
    Orientation orientation = Orientation::vertical;
};

class SuperColliderTokeniser final : public juce::CodeTokeniser
{
public:
    enum TokenType
    {
        error = 0,
        comment,
        keyword,
        ugen,
        identifier,
        number,
        string,
        symbol,
        bracket,
        punctuation,
        op
    };

    int readNextToken (juce::CodeDocument::Iterator& source) override
    {
        source.skipWhitespace();

        const auto first = source.peekNextChar();
        if (first == 0)
            return identifier;

        if (first == '/' && source.peekPreviousChar() != '\\')
        {
            source.skip();
            if (source.peekNextChar() == '/')
            {
                source.skipToEndOfLine();
                return comment;
            }

            return op;
        }

        if (first == '"')
        {
            source.skip();
            while (! source.isEOF())
            {
                const auto c = source.nextChar();
                if (c == '\\')
                    source.skip();
                else if (c == '"')
                    break;
            }
            return string;
        }

        if (first == '\\')
        {
            source.skip();
            while (isIdentifierBody (source.peekNextChar()))
                source.skip();
            return symbol;
        }

        if (juce::CharacterFunctions::isDigit (first))
        {
            bool seenDot = false;
            while (juce::CharacterFunctions::isDigit (source.peekNextChar()) || (! seenDot && source.peekNextChar() == '.'))
            {
                seenDot = seenDot || source.peekNextChar() == '.';
                source.skip();
            }
            return number;
        }

        if (isIdentifierStart (first))
        {
            juce::String token;
            while (isIdentifierBody (source.peekNextChar()))
                token << juce::String::charToString (source.nextChar());

            if (isKeyword (token))
                return keyword;
            if (isUGen (token))
                return ugen;
            return identifier;
        }

        if (isBracket (first))
        {
            source.skip();
            return bracket;
        }

        if (juce::String (";,.").containsChar (first))
        {
            source.skip();
            return punctuation;
        }

        source.skip();
        return op;
    }

    juce::CodeEditorComponent::ColourScheme getDefaultColourScheme() override
    {
        juce::CodeEditorComponent::ColourScheme scheme;
        scheme.set ("Error",       juce::Colour (0xffff5c77));
        scheme.set ("Comment",     juce::Colour (0xff68737d));
        scheme.set ("Keyword",     juce::Colour (0xffffc857));
        scheme.set ("UGen",        juce::Colour (0xff52d1dc));
        scheme.set ("Identifier",  ink());
        scheme.set ("Number",      juce::Colour (0xff7bd88f));
        scheme.set ("String",      juce::Colour (0xfff76f8e));
        scheme.set ("Symbol",      juce::Colour (0xffb48cff));
        scheme.set ("Bracket",     juce::Colour (0xfff2efe7));
        scheme.set ("Punctuation", juce::Colour (0xffaeb5bd));
        scheme.set ("Operator",    juce::Colour (0xffff9f68));
        return scheme;
    }

private:
    static bool isIdentifierStart (juce::juce_wchar c)
    {
        return juce::CharacterFunctions::isLetter (c) || c == '_' || c == '~';
    }

    static bool isIdentifierBody (juce::juce_wchar c)
    {
        return isIdentifierStart (c) || juce::CharacterFunctions::isDigit (c);
    }

    static bool isBracket (juce::juce_wchar c)
    {
        return c == '(' || c == ')' || c == '{' || c == '}' || c == '[' || c == ']';
    }

    static bool isKeyword (const juce::String& token)
    {
        static const char* keywords[] =
        {
            "arg", "var", "classvar", "const", "this", "super", "nil", "true", "false",
            "if", "while", "for", "case", "switch", "do", "collect", "select", "reject",
            "inf", "pi"
        };

        for (auto* keyword : keywords)
            if (token == keyword)
                return true;

        return false;
    }

    static bool isUGen (const juce::String& token)
    {
        static const char* ugens[] =
        {
            "SinOsc", "LFTri", "LFSaw", "VarSaw", "Pulse", "Saw", "WhiteNoise", "PinkNoise",
            "Impulse", "Demand", "Dseq", "Dwhite", "Drand", "Env", "EnvGen", "Decay2",
            "Lag", "TRand", "LFNoise0", "LFNoise1", "RLPF", "LPF", "HPF", "BPF",
            "Limiter", "LeakDC", "Compander", "Pan2", "Splay", "CombC", "Mix",
            "In", "ReplaceOut", "Out", "SendReply", "Amplitude", "Silent"
        };

        for (auto* ugenName : ugens)
            if (token == ugenName)
                return true;

        return false;
    }
};

class SuperColliderCodeEditor final : public juce::CodeEditorComponent,
                                      private juce::Timer
{
public:
    SuperColliderCodeEditor (juce::CodeDocument& document, juce::CodeTokeniser* tokeniser)
        : juce::CodeEditorComponent (document, tokeniser)
    {
        startTimerHz (20);
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        drawCurrentLine (g);
        drawBracketMatch (g);
    }

    void caretPositionMoved() override
    {
        repaint();
    }

private:
    void timerCallback() override
    {
        repaint();
    }

    void drawCurrentLine (juce::Graphics& g)
    {
        const auto caret = getCaretPos();
        const auto bounds = getCharacterBounds ({ getDocument(), caret.getLineNumber(), 0 });
        if (bounds.isEmpty())
            return;

        g.setColour (accentB().withAlpha (0.055f));
        g.fillRect (juce::Rectangle<int> (0, bounds.getY(), getWidth(), getLineHeight()));
    }

    void drawBracketMatch (juce::Graphics& g)
    {
        const auto text = getDocument().getAllContent();
        if (text.isEmpty())
            return;

        const auto caretIndex = getCaretPosition();
        const auto bracketIndex = findBracketNearCaret (text, caretIndex);
        if (bracketIndex < 0)
            return;

        const auto matchIndex = findMatchingBracket (text, bracketIndex);
        drawBracketBox (g, bracketIndex, matchIndex >= 0 ? accentA() : accentC());

        if (matchIndex >= 0)
            drawBracketBox (g, matchIndex, accentA());
    }

    int findBracketNearCaret (const juce::String& text, int caretIndex) const
    {
        if (caretIndex > 0 && isBracket (text[caretIndex - 1]))
            return caretIndex - 1;

        if (caretIndex < text.length() && isBracket (text[caretIndex]))
            return caretIndex;

        return -1;
    }

    int findMatchingBracket (const juce::String& text, int bracketIndex) const
    {
        const auto open = text[bracketIndex];
        const auto close = matchingBracket (open);
        if (close == 0)
            return -1;

        const auto direction = isOpeningBracket (open) ? 1 : -1;
        const auto targetOpen = direction > 0 ? open : close;
        const auto targetClose = direction > 0 ? close : open;
        int depth = 0;

        for (int i = bracketIndex; i >= 0 && i < text.length(); i += direction)
        {
            const auto c = text[i];
            if (c == targetOpen)
                ++depth;
            else if (c == targetClose)
            {
                --depth;
                if (depth == 0)
                    return i;
            }
        }

        return -1;
    }

    void drawBracketBox (juce::Graphics& g, int index, juce::Colour colour)
    {
        auto bounds = getCharacterBounds ({ getDocument(), index }).toFloat().expanded (1.5f, 1.0f);
        if (bounds.isEmpty())
            return;

        g.setColour (colour.withAlpha (0.18f));
        g.fillRoundedRectangle (bounds, 2.0f);
        g.setColour (colour.withAlpha (0.90f));
        g.drawRoundedRectangle (bounds, 2.0f, 1.1f);
    }

    static bool isBracket (juce::juce_wchar c)
    {
        return c == '(' || c == ')' || c == '{' || c == '}' || c == '[' || c == ']';
    }

    static bool isOpeningBracket (juce::juce_wchar c)
    {
        return c == '(' || c == '{' || c == '[';
    }

    static juce::juce_wchar matchingBracket (juce::juce_wchar c)
    {
        switch (c)
        {
            case '(': return ')';
            case '{': return '}';
            case '[': return ']';
            case ')': return '(';
            case '}': return '{';
            case ']': return '[';
            default: break;
        }

        return 0;
    }
};

class MainComponent final : public juce::Component,
                            private juce::CodeDocument::Listener,
                            private juce::OSCReceiver,
                            private juce::OSCReceiver::ListenerWithOSCAddress<juce::OSCReceiver::MessageLoopCallback>
{
public:
    MainComponent() : graph (machine), rules (machine), scriptEditor (codeDocument, &scTokeniser)
    {
        setSize (1180, 760);

        addAndMakeVisible (title);
        addAndMakeVisible (statusLabel);
        addAndMakeVisible (logButton);
        addAndMakeVisible (panicButton);
        addAndMakeVisible (topStateCountLabel);
        addAndMakeVisible (topStateCountMinus);
        addAndMakeVisible (topStateCountEditor);
        addAndMakeVisible (topStateCountPlus);
        addAndMakeVisible (runButton);
        addAndMakeVisible (stepButton);
        addAndMakeVisible (stopAllButton);
        addAndMakeVisible (rateSlider);
        addAndMakeVisible (graph);
        addAndMakeVisible (graphFitButton);
        addAndMakeVisible (graphLayoutButton);
        addAndMakeVisible (rules);
        addAndMakeVisible (graphBottomDivider);
        addAndMakeVisible (rulesTracksDivider);
        addAndMakeVisible (tracksCodeDivider);
        addAndMakeVisible (stateTabs);
        addAndMakeVisible (breadcrumbLabel);
        addAndMakeVisible (stateSummaryLabel);
        addAndMakeVisible (stateTempoLabel);
        addAndMakeVisible (stateTempoEditor);
        addAndMakeVisible (stateMeterLabel);
        addAndMakeVisible (stateMeterBeatsEditor);
        addAndMakeVisible (stateMeterSlashLabel);
        addAndMakeVisible (stateMeterUnitEditor);
        addAndMakeVisible (nestedTimingLabel);
        addAndMakeVisible (nestedModeBox);
        addAndMakeVisible (nestedDivisionLabel);
        addAndMakeVisible (nestedDivisionMinus);
        addAndMakeVisible (nestedDivisionEditor);
        addAndMakeVisible (nestedDivisionPlus);
        addAndMakeVisible (trackPaneTitle);
        addAndMakeVisible (trackNameEditor);
        addAndMakeVisible (trackList);
        addAndMakeVisible (codePaneTitle);
        addAndMakeVisible (codeStatsLabel);
        addAndMakeVisible (codeCheckLabel);
        addAndMakeVisible (checkCodeButton);
        addAndMakeVisible (codeFontDownButton);
        addAndMakeVisible (codeFontUpButton);
        addAndMakeVisible (tidyCodeButton);
        addAndMakeVisible (expandCodeButton);
        addAndMakeVisible (scriptEditor);
        addAndMakeVisible (addLaneButton);
        addAndMakeVisible (removeLaneButton);
        addAndMakeVisible (moveLaneUpButton);
        addAndMakeVisible (moveLaneDownButton);
        addAndMakeVisible (addChildMachineButton);
        addAndMakeVisible (enterChildMachineButton);
        addAndMakeVisible (exitChildMachineButton);
        addAndMakeVisible (playButton);
        addAndMakeVisible (stopButton);

        title.setText ("Markov FSM", juce::dontSendNotification);
        title.setFont (juce::FontOptions (24.0f, juce::Font::bold));
        title.setColour (juce::Label::textColourId, ink());

        breadcrumbLabel.setFont (juce::FontOptions (12.0f));
        breadcrumbLabel.setColour (juce::Label::textColourId, mutedInk());
        breadcrumbLabel.setJustificationType (juce::Justification::centredLeft);

        stateSummaryLabel.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        stateSummaryLabel.setColour (juce::Label::textColourId, ink());
        stateSummaryLabel.setJustificationType (juce::Justification::centredLeft);

        stateTempoLabel.setText ("Tempo", juce::dontSendNotification);
        stateTempoLabel.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        stateTempoLabel.setColour (juce::Label::textColourId, mutedInk());
        stateMeterLabel.setText ("Meter", juce::dontSendNotification);
        stateMeterLabel.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        stateMeterLabel.setColour (juce::Label::textColourId, mutedInk());
        stateMeterSlashLabel.setText ("/", juce::dontSendNotification);
        stateMeterSlashLabel.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        stateMeterSlashLabel.setColour (juce::Label::textColourId, mutedInk());
        stateMeterSlashLabel.setJustificationType (juce::Justification::centred);

        configureSmallNumberEditor (stateTempoEditor, 6, "0123456789.");
        configureSmallNumberEditor (stateMeterBeatsEditor, 2, "0123456789");
        configureSmallNumberEditor (stateMeterUnitEditor, 2, "0123456789");
        stateTempoEditor.onReturnKey = [this] { commitStateTimingEditors(); };
        stateTempoEditor.onFocusLost = [this] { commitStateTimingEditors(); };
        stateMeterBeatsEditor.onReturnKey = [this] { commitStateTimingEditors(); };
        stateMeterBeatsEditor.onFocusLost = [this] { commitStateTimingEditors(); };
        stateMeterUnitEditor.onReturnKey = [this] { commitStateTimingEditors(); };
        stateMeterUnitEditor.onFocusLost = [this] { commitStateTimingEditors(); };

        nestedTimingLabel.setText ("Nested timing", juce::dontSendNotification);
        nestedTimingLabel.setFont (juce::FontOptions (12.5f, juce::Font::bold));
        nestedTimingLabel.setColour (juce::Label::textColourId, mutedInk());

        nestedModeBox.addItem (nestedTimingModeName (NestedTimingMode::followParent), 1);
        nestedModeBox.addItem (nestedTimingModeName (NestedTimingMode::freeRun), 2);
        nestedModeBox.addItem (nestedTimingModeName (NestedTimingMode::oneShot), 3);
        nestedModeBox.addItem (nestedTimingModeName (NestedTimingMode::latch), 4);
        nestedModeBox.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff252a31));
        nestedModeBox.setColour (juce::ComboBox::textColourId, ink());
        nestedModeBox.setColour (juce::ComboBox::outlineColourId, juce::Colour (0xff59636e));
        nestedModeBox.onChange = [this]
        {
            if (auto* child = currentInspectorMachine().childMachine (currentInspectorMachine().selectedState))
            {
                child->timingMode = static_cast<NestedTimingMode> (juce::jlimit (0, 3, nestedModeBox.getSelectedItemIndex()));
                child->oneShotComplete = false;
                markMachineDirty();
                refreshControls();
            }
        };

        nestedDivisionLabel.setText ("Division", juce::dontSendNotification);
        nestedDivisionLabel.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        nestedDivisionLabel.setColour (juce::Label::textColourId, mutedInk());
        nestedDivisionMinus.setButtonText ("-");
        nestedDivisionPlus.setButtonText ("+");
        nestedDivisionEditor.setInputRestrictions (2, "0123456789");
        nestedDivisionEditor.setJustification (juce::Justification::centred);
        nestedDivisionEditor.setMultiLine (false);
        nestedDivisionEditor.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff111318));
        nestedDivisionEditor.setColour (juce::TextEditor::textColourId, ink());
        nestedDivisionEditor.setColour (juce::TextEditor::outlineColourId, juce::Colour (0xff34414a));
        nestedDivisionEditor.onReturnKey = [this] { commitNestedDivisionEditor(); };
        nestedDivisionEditor.onFocusLost = [this] { commitNestedDivisionEditor(); };
        nestedDivisionMinus.onClick = [this] { adjustNestedDivision (-1); };
        nestedDivisionPlus.onClick = [this] { adjustNestedDivision (1); };

        codePaneTitle.setText ("SC Code", juce::dontSendNotification);
        codePaneTitle.setFont (juce::FontOptions (13.5f, juce::Font::bold));
        codePaneTitle.setColour (juce::Label::textColourId, ink());
        codePaneTitle.setJustificationType (juce::Justification::centredLeft);
        codeStatsLabel.setFont (juce::FontOptions (11.5f));
        codeStatsLabel.setColour (juce::Label::textColourId, mutedInk());
        codeStatsLabel.setJustificationType (juce::Justification::centredRight);
        codeCheckLabel.setText ("Not checked", juce::dontSendNotification);
        codeCheckLabel.setFont (juce::FontOptions (11.5f, juce::Font::bold));
        codeCheckLabel.setColour (juce::Label::textColourId, mutedInk());
        codeCheckLabel.setJustificationType (juce::Justification::centredLeft);

        trackPaneTitle.setText ("Tracks", juce::dontSendNotification);
        trackPaneTitle.setFont (juce::FontOptions (13.5f, juce::Font::bold));
        trackPaneTitle.setColour (juce::Label::textColourId, ink());
        trackPaneTitle.setJustificationType (juce::Justification::centredLeft);

        trackNameEditor.setMultiLine (false);
        trackNameEditor.setFont (juce::FontOptions (13.0f));
        trackNameEditor.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff111318));
        trackNameEditor.setColour (juce::TextEditor::textColourId, ink());
        trackNameEditor.setColour (juce::TextEditor::outlineColourId, juce::Colour (0xff34414a));
        trackNameEditor.onTextChange = [this]
        {
            currentInspectorMachine().selectedLaneRef().name = trackNameEditor.getText().trim();
            markMachineDirty();
            trackList.repaint();
        };

        statusLabel.setText ("Audio offline", juce::dontSendNotification);
        statusLabel.setFont (juce::FontOptions (13.0f));
        statusLabel.setColour (juce::Label::textColourId, mutedInk());
        statusLabel.setJustificationType (juce::Justification::centredRight);
        statusLabel.setMouseCursor (juce::MouseCursor::PointingHandCursor);
        statusLabel.onClick = [this]
        {
            startPrepareJob (false);
        };

        rulesTracksDivider.onDragged = [this] (int delta)
        {
            rulesPaneWidth = juce::jlimit (300, 860, dividerDragStartRulesWidth + delta);
            resized();
        };
        rulesTracksDivider.onDragStarted = [this]
        {
            rulesPaneUserSized = true;
            dividerDragStartRulesWidth = rulesPaneWidth;
        };

        tracksCodeDivider.onDragged = [this] (int delta)
        {
            tracksPaneWidth = juce::jlimit (260, 460, dividerDragStartTracksWidth - delta);
            resized();
        };
        tracksCodeDivider.onDragStarted = [this]
        {
            tracksPaneUserSized = true;
            dividerDragStartTracksWidth = tracksPaneWidth;
        };

        graphBottomDivider.onDragged = [this] (int delta)
        {
            bottomPaneHeight = dividerDragStartBottomHeight - delta;
            resized();
        };
        graphBottomDivider.onDragStarted = [this]
        {
            bottomPaneUserSized = true;
            dividerDragStartBottomHeight = bottomPaneHeight;
            graph.beginNodePositionLock();
        };
        graphBottomDivider.onDragEnded = [this]
        {
            graph.endNodePositionLock();
        };

        logButton.setButtonText ("Log");
        panicButton.setButtonText ("Panic");
        panicButton.setColour (juce::TextButton::buttonColourId, accentC().darker (0.45f));

        logView.setMultiLine (true);
        logView.setReadOnly (true);
        logView.setScrollbarsShown (true);
        logView.setCaretVisible (false);
        logView.setFont (juce::FontOptions (12.5f));
        logView.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xee111318));
        logView.setColour (juce::TextEditor::textColourId, mutedInk());
        logView.setColour (juce::TextEditor::outlineColourId, juce::Colour (0xff333a44));
        addChildComponent (logView);

        host.onStatusChanged = [this] (const juce::String& status)
        {
            statusLabel.setText (status, juce::dontSendNotification);
        };

        host.onLogMessage = [this] (const juce::String& message)
        {
            handleHostLogMessage (message);
            appendLog (message);
        };

        if (connect (57142))
            addListener (this, "/markov/state");
        else
            appendLog ("Could not bind visual state OSC port 57142");

        topStateCountLabel.setText ("Top states", juce::dontSendNotification);
        topStateCountLabel.setFont (juce::FontOptions (12.5f, juce::Font::bold));
        topStateCountLabel.setColour (juce::Label::textColourId, mutedInk());
        topStateCountLabel.setJustificationType (juce::Justification::centredRight);

        topStateCountMinus.setButtonText ("-");
        topStateCountPlus.setButtonText ("+");
        topStateCountMinus.onClick = [this] { setTopLevelStateCount (machine.getStateCount() - 1); };
        topStateCountPlus.onClick = [this] { setTopLevelStateCount (machine.getStateCount() + 1); };

        topStateCountEditor.setText (juce::String (machine.getStateCount()), false);
        topStateCountEditor.setInputRestrictions (2, "0123456789");
        topStateCountEditor.setJustification (juce::Justification::centred);
        topStateCountEditor.setSelectAllWhenFocused (true);
        topStateCountEditor.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff181b20));
        topStateCountEditor.setColour (juce::TextEditor::textColourId, ink());
        topStateCountEditor.setColour (juce::TextEditor::outlineColourId, juce::Colour (0xff333a44));
        topStateCountEditor.setColour (juce::TextEditor::focusedOutlineColourId, accentA());
        topStateCountEditor.onReturnKey = [this] { commitTopLevelStateCountEditor(); };
        topStateCountEditor.onFocusLost = [this] { commitTopLevelStateCountEditor(); };

        runButton.setButtonText ("Run FSM");
        stepButton.setButtonText ("Step");
        stopAllButton.setButtonText ("Stop all");
        rateSlider.setRange (0.2, 4.0, 0.1);
        rateSlider.setValue (0.25);
        rateSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 48, 22);

        graphFitButton.setButtonText ("Fit");
        graphLayoutButton.setButtonText ("Layout");
        graphFitButton.onClick = [this] { graph.fitView(); };
        graphLayoutButton.onClick = [this] { graph.resetLayout(); };

        runButton.onClick = [this]
        {
            if (! fsmRunning)
            {
                fsmRunning = true;
                if (machinePrepared && ! audioJobRunning)
                    startPreparedRun();
                else
                    startPrepareJob (true);
            }
            else
            {
                fsmRunning = false;
                stopTransport();
                host.pauseMachine();
                host.stopAll (machine);
                runButton.setButtonText ("Run FSM");
            }
        };

        stepButton.onClick = [this]
        {
            host.stepMachine();
        };

        stopAllButton.onClick = [this]
        {
            fsmRunning = false;
            stopTransport();
            runButton.setButtonText ("Run FSM");
            host.pauseMachine();
            host.stopAll (machine);
            refreshControls();
        };

        panicButton.onClick = [this]
        {
            fsmRunning = false;
            stopTransport();
            host.pauseMachine();
            runButton.setButtonText ("Run FSM");
            host.panic (machine);
            refreshControls();
        };

        logButton.onClick = [this]
        {
            logVisible = ! logVisible;
            logView.setVisible (logVisible);
            resized();
        };

        rateSlider.onValueChange = [this]
        {
            transportIntervalMs = getTransportIntervalMs();
            if (fsmRunning)
            {
                restartTransport();
            }
        };

        stateTabs.onIndexSelected = [this] (int newIndex)
        {
            currentMachine().selectedState = newIndex;
            currentMachine().selectedLane = 0;
            inspectedMachine = &currentMachine();
            refreshControls();
        };

        trackList.onTrackSelected = [this] (int newIndex)
        {
            currentInspectorMachine().selectedLane = newIndex;
            refreshControls();
        };

        trackList.onEnabledToggled = [this] (int newIndex)
        {
            auto& inspected = currentInspectorMachine();
            inspected.selectedLane = newIndex;
            auto& lane = inspected.selectedLaneRef();
            lane.enabled = ! lane.enabled;
            if (! lane.enabled)
                host.stop (lane);
            markMachineDirty();
            refreshControls();
        };

        trackList.onMuteToggled = [this] (int newIndex)
        {
            auto& inspected = currentInspectorMachine();
            inspected.selectedLane = newIndex;
            auto& lane = inspected.selectedLaneRef();
            lane.muted = ! lane.muted;
            if (lane.muted)
                host.stop (lane);
            markMachineDirty();
            refreshControls();
        };

        trackList.onSoloToggled = [this] (int newIndex)
        {
            auto& inspected = currentInspectorMachine();
            inspected.selectedLane = newIndex;
            inspected.selectedLaneRef().solo = ! inspected.selectedLaneRef().solo;
            markMachineDirty();
            refreshControls();
        };

        trackList.onVolumeChanged = [this] (int newIndex, float volume)
        {
            auto& inspected = currentInspectorMachine();
            inspected.selectedLane = newIndex;
            auto& lane = inspected.selectedLaneRef();
            lane.volume = juce::jlimit (0.0f, 1.0f, volume);
            host.setLaneVolume (lane);
            refreshControls();
        };

        codeDocument.addListener (this);
        scriptEditor.setLineNumbersShown (true);
        scriptEditor.setTabSize (4, true);
        scriptEditor.setScrollbarThickness (9);
        updateCodeEditorFont();
        scriptEditor.setColour (juce::CodeEditorComponent::backgroundColourId, juce::Colour (0xff0f1116));
        scriptEditor.setColour (juce::CodeEditorComponent::defaultTextColourId, ink());
        scriptEditor.setColour (juce::CodeEditorComponent::highlightColourId, accentB().withAlpha (0.24f));
        scriptEditor.setColour (juce::CodeEditorComponent::lineNumberBackgroundId, juce::Colour (0xff111820));
        scriptEditor.setColour (juce::CodeEditorComponent::lineNumberTextId, mutedInk().withAlpha (0.52f));
        scriptEditor.setColourScheme (scTokeniser.getDefaultColourScheme());

        codeFontDownButton.setButtonText ("A-");
        codeFontUpButton.setButtonText ("A+");
        tidyCodeButton.setButtonText ("Tidy");
        checkCodeButton.setButtonText ("Check");
        expandCodeButton.setButtonText ("Expand");
        codeFontDownButton.onClick = [this]
        {
            codeFontSize = juce::jlimit (11.0f, 20.0f, codeFontSize - 1.0f);
            updateCodeEditorFont();
        };
        codeFontUpButton.onClick = [this]
        {
            codeFontSize = juce::jlimit (11.0f, 20.0f, codeFontSize + 1.0f);
            updateCodeEditorFont();
        };
        tidyCodeButton.onClick = [this]
        {
            tidySelectedLaneScript();
        };
        checkCodeButton.onClick = [this]
        {
            checkSelectedLaneScript();
        };
        expandCodeButton.onClick = [this]
        {
            codeExpanded = ! codeExpanded;
            expandCodeButton.setButtonText (codeExpanded ? "Shrink" : "Expand");
            resized();
            scriptEditor.grabKeyboardFocus();
        };

        addLaneButton.setButtonText ("+ Lane");
        removeLaneButton.setButtonText ("- Lane");
        moveLaneUpButton.setButtonText ("Up");
        moveLaneDownButton.setButtonText ("Down");
        addChildMachineButton.setButtonText ("+ FSM");
        enterChildMachineButton.setButtonText ("Enter");
        exitChildMachineButton.setButtonText ("Back");
        playButton.setButtonText ("Play");
        stopButton.setButtonText ("Stop");

        addLaneButton.onClick = [this]
        {
            currentInspectorMachine().addLaneToSelectedState();
            markMachineDirty();
            refreshControls();
        };

        removeLaneButton.onClick = [this]
        {
            host.stop (currentInspectorMachine().selectedLaneRef());
            currentInspectorMachine().removeSelectedLane();
            markMachineDirty();
            refreshControls();
        };

        moveLaneUpButton.onClick = [this]
        {
            currentInspectorMachine().moveSelectedLane (-1);
            markMachineDirty();
            refreshControls();
        };

        moveLaneDownButton.onClick = [this]
        {
            currentInspectorMachine().moveSelectedLane (1);
            markMachineDirty();
            refreshControls();
        };

        addChildMachineButton.onClick = [this]
        {
            currentInspectorMachine().addChildToSelectedState();
            markMachineDirty();
            refreshControls();
        };

        enterChildMachineButton.onClick = [this]
        {
            auto& inspected = currentInspectorMachine();
            if (auto* child = inspected.childMachine (inspected.selectedState))
            {
                machineStack.push_back (&inspected);
                setActiveMachine (*child);
            }
        };

        exitChildMachineButton.onClick = [this]
        {
            if (! machineStack.empty())
            {
                auto* parent = machineStack.back();
                machineStack.pop_back();
                setActiveMachine (*parent);
            }
        };

        playButton.onClick = [this]
        {
            auto& inspected = currentInspectorMachine();
            auto& state = inspected.state (inspected.selectedState);
            auto& lane = inspected.selectedLaneRef();
            if (shouldPlayLane (state, lane))
                host.play (lane, getSclangPathOverride());
            else
                host.stop (lane);
            refreshControls();
        };

        stopButton.onClick = [this]
        {
            host.stop (currentInspectorMachine().selectedLaneRef());
            refreshControls();
        };

        graph.onStateChosen = [this] (int)
        {
            inspectedMachine = &currentMachine();
            refreshControls();
        };

        graph.onNestedBadgeChosen = [this] (int parentStateIndex)
        {
            if (auto* child = currentMachine().childMachine (parentStateIndex))
            {
                currentMachine().selectedState = parentStateIndex;
                child->selectedLane = 0;
                inspectedMachine = child;
                refreshControls();
            }
        };

        graph.onNestedStateChosen = [this] (int parentStateIndex, int childStateIndex)
        {
            if (auto* child = currentMachine().childMachine (parentStateIndex))
            {
                currentMachine().selectedState = parentStateIndex;
                child->selectedState = childStateIndex;
                child->selectedLane = 0;
                inspectedMachine = child;
                refreshControls();
            }
        };

        graph.onSecondLayerNestedStateChosen = [this] (int parentStateIndex, int childStateIndex, int grandchildStateIndex)
        {
            if (auto* child = currentMachine().childMachine (parentStateIndex))
            {
                currentMachine().selectedState = parentStateIndex;
                child->selectedState = childStateIndex;
                child->selectedLane = 0;

                if (auto* grandchild = child->childMachine (childStateIndex))
                {
                    grandchild->selectedState = grandchildStateIndex;
                    grandchild->selectedLane = 0;
                    inspectedMachine = grandchild;
                }
                else
                {
                    inspectedMachine = child;
                }

                refreshControls();
            }
        };

        graph.onNestedStateCountChanged = [this] (int parentStateIndex, int newCount)
        {
            if (auto* child = currentMachine().childMachine (parentStateIndex))
            {
                fsmRunning = false;
                stopTransport();
                host.stopAll (machine);
                runButton.setButtonText ("Run FSM");

                child->setStateCount (newCount);
                child->regenerateRingRules();
                markMachineDirty();
                refreshControls();
            }
        };

        graph.onSecondLayerNestedStateCountChanged = [this] (int parentStateIndex, int childStateIndex, int newCount)
        {
            if (auto* child = currentMachine().childMachine (parentStateIndex))
            {
                if (auto* grandchild = child->childMachine (childStateIndex))
                {
                    fsmRunning = false;
                    stopTransport();
                    host.stopAll (machine);
                    runButton.setButtonText ("Run FSM");

                    grandchild->setStateCount (newCount);
                    grandchild->regenerateRingRules();
                    markMachineDirty();
                    refreshControls();
                }
            }
        };

        rules.onRulesChanged = [this]
        {
            markMachineDirty();
            graph.repaint();
            rules.repaint();
        };

        refreshControls();
        juce::Timer::callAfterDelay (350, [safeThis = juce::Component::SafePointer<MainComponent> (this)]
        {
            if (safeThis != nullptr)
                safeThis->startPrepareJob (false);
        });
    }

    ~MainComponent() override
    {
        codeDocument.removeListener (this);
        removeListener (this);
        disconnect();
        stopTransport();
        stopMachineRecursive (machine);
        host.onLogMessage = nullptr;
        host.onStatusChanged = nullptr;
    }

    void paint (juce::Graphics& g) override
    {
        juce::ColourGradient bg (juce::Colour (0xff0f1013), getLocalBounds().getTopLeft().toFloat(),
                                 juce::Colour (0xff242830), getLocalBounds().getBottomRight().toFloat(), false);
        g.setGradientFill (bg);
        g.fillAll();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (18);
        auto header = area.removeFromTop (46);
        title.setBounds (header.removeFromLeft (360));
        auto topCountArea = header.removeFromRight (188).reduced (0, 8);
        topStateCountLabel.setBounds (topCountArea.removeFromLeft (76));
        topStateCountMinus.setBounds (topCountArea.removeFromLeft (28).reduced (2, 0));
        topStateCountEditor.setBounds (topCountArea.removeFromLeft (42).reduced (2, 0));
        topStateCountPlus.setBounds (topCountArea.removeFromLeft (28).reduced (2, 0));
        rateSlider.setBounds (header.removeFromRight (150).reduced (6, 8));
        graphLayoutButton.setBounds (header.removeFromRight (68).reduced (4, 8));
        graphFitButton.setBounds (header.removeFromRight (46).reduced (4, 8));
        logButton.setBounds (header.removeFromRight (56).reduced (4, 8));
        panicButton.setBounds (header.removeFromRight (74).reduced (4, 8));
        stopAllButton.setBounds (header.removeFromRight (86).reduced (4, 8));
        stepButton.setBounds (header.removeFromRight (66).reduced (4, 8));
        runButton.setBounds (header.removeFromRight (88).reduced (4, 8));
        statusLabel.setBounds (header.reduced (8, 8));

        const auto horizontalDividerHeight = 8;
        const auto minGraphHeight = codeExpanded ? 112 : 230;
        const auto minBottomHeight = codeExpanded ? 320 : 170;
        const auto maxBottomHeight = juce::jmax (minBottomHeight, area.getHeight() - 36 - minGraphHeight - horizontalDividerHeight);
        if (codeExpanded)
        {
            bottomPaneHeight = juce::jlimit (minBottomHeight, maxBottomHeight,
                                             juce::roundToInt (static_cast<float> (area.getHeight()) * 0.58f));
        }
        else
        {
            if (! bottomPaneUserSized)
                bottomPaneHeight = juce::jlimit (240, 330, juce::roundToInt (static_cast<float> (area.getHeight()) * 0.30f));
            bottomPaneHeight = juce::jlimit (minBottomHeight, maxBottomHeight, bottomPaneHeight);
        }

        const auto dividerWidth = 8;
        const auto minWorkspace = 760;
        const auto minTracks = 260;
        const auto maxTracks = juce::jmax (minTracks, area.getWidth() - minWorkspace - dividerWidth);
        if (! tracksPaneUserSized)
            tracksPaneWidth = juce::jlimit (300, 360, juce::roundToInt (static_cast<float> (area.getWidth()) * 0.17f));
        tracksPaneWidth = juce::jlimit (minTracks, juce::jmin (460, maxTracks), tracksPaneWidth);

        auto tracksPane = area.removeFromRight (tracksPaneWidth);
        tracksCodeDivider.setBounds (area.removeFromRight (dividerWidth).reduced (0, 8));
        auto workspace = area;

        auto lower = workspace.removeFromBottom (bottomPaneHeight).reduced (0, 8);
        graphBottomDivider.setBounds (workspace.removeFromBottom (horizontalDividerHeight).reduced (0, 1));
        const auto minRules = codeExpanded ? 190 : 300;
        const auto minCode = 440;
        const auto maxRules = juce::jmax (minRules, lower.getWidth() - minCode - dividerWidth);
        if (codeExpanded)
            rulesPaneWidth = juce::roundToInt (static_cast<float> (lower.getWidth()) * 0.26f);
        else if (! rulesPaneUserSized)
            rulesPaneWidth = juce::roundToInt (static_cast<float> (lower.getWidth()) * 0.43f);
        rulesPaneWidth = juce::jlimit (minRules, maxRules, rulesPaneWidth);

        auto bottom = lower;
        auto rulesPane = bottom.removeFromLeft (rulesPaneWidth);
        auto dividerA = bottom.removeFromLeft (dividerWidth);
        auto codePane = bottom;

        rules.setBounds (rulesPane);
        rulesTracksDivider.setBounds (dividerA);

        auto trackPaneInner = tracksPane.reduced (10, 8);
        breadcrumbLabel.setBounds (trackPaneInner.removeFromTop (22).reduced (2, 0));
        stateSummaryLabel.setBounds (trackPaneInner.removeFromTop (28).reduced (2, 0));
        auto stateTimingRow = trackPaneInner.removeFromTop (34);
        stateTempoLabel.setBounds (stateTimingRow.removeFromLeft (54).reduced (2, 4));
        stateTempoEditor.setBounds (stateTimingRow.removeFromLeft (66).reduced (2, 4));
        stateTimingRow.removeFromLeft (8);
        stateMeterLabel.setBounds (stateTimingRow.removeFromLeft (48).reduced (2, 4));
        stateMeterBeatsEditor.setBounds (stateTimingRow.removeFromLeft (36).reduced (2, 4));
        stateMeterSlashLabel.setBounds (stateTimingRow.removeFromLeft (14).reduced (0, 4));
        stateMeterUnitEditor.setBounds (stateTimingRow.removeFromLeft (36).reduced (2, 4));
        trackPaneInner.removeFromTop (4);
        auto timingRow = trackPaneInner.removeFromTop (34);
        nestedTimingLabel.setBounds (timingRow.removeFromLeft (96).reduced (2, 4));
        nestedModeBox.setBounds (timingRow.reduced (2, 4));
        auto divisionRow = trackPaneInner.removeFromTop (32);
        nestedDivisionLabel.setBounds (divisionRow.removeFromLeft (76).reduced (2, 4));
        nestedDivisionMinus.setBounds (divisionRow.removeFromLeft (28).reduced (2, 4));
        nestedDivisionEditor.setBounds (divisionRow.removeFromLeft (42).reduced (2, 4));
        nestedDivisionPlus.setBounds (divisionRow.removeFromLeft (28).reduced (2, 4));
        trackPaneInner.removeFromTop (8);
        auto trackNameRow = trackPaneInner.removeFromTop (34);
        trackNameEditor.setBounds (trackNameRow.reduced (0, 2));
        auto trackHeader = trackPaneInner.removeFromTop (38);
        trackPaneTitle.setBounds (trackHeader.removeFromLeft (68).reduced (2, 4));
        moveLaneUpButton.setBounds (trackHeader.removeFromRight (54).reduced (3, 4));
        moveLaneDownButton.setBounds (trackHeader.removeFromRight (64).reduced (3, 4));
        auto laneButtonRow = trackPaneInner.removeFromTop (38);
        addLaneButton.setBounds (laneButtonRow.removeFromLeft (92).reduced (0, 4));
        removeLaneButton.setBounds (laneButtonRow.removeFromLeft (92).reduced (6, 4));
        trackList.setBounds (trackPaneInner.reduced (0, 4));

        auto codePaneInner = codePane.reduced (8, 0);
        auto codeHeader = codePaneInner.removeFromTop (34);
        codePaneTitle.setBounds (codeHeader.removeFromLeft (74).reduced (3));
        codeStatsLabel.setBounds (codeHeader.removeFromRight (132).reduced (3));
        codeFontUpButton.setBounds (codeHeader.removeFromRight (42).reduced (3));
        codeFontDownButton.setBounds (codeHeader.removeFromRight (42).reduced (3));
        tidyCodeButton.setBounds (codeHeader.removeFromRight (58).reduced (3));
        expandCodeButton.setBounds (codeHeader.removeFromRight (72).reduced (3));
        codeCheckLabel.setBounds (codeHeader.removeFromRight (codeExpanded ? 280 : 170).reduced (3));
        addChildMachineButton.setBounds (codeHeader.removeFromLeft (64).reduced (3));
        enterChildMachineButton.setBounds (codeHeader.removeFromLeft (58).reduced (3));
        exitChildMachineButton.setBounds (codeHeader.removeFromLeft (54).reduced (3));
        checkCodeButton.setBounds (codeHeader.removeFromLeft (66).reduced (3));
        playButton.setBounds (codeHeader.removeFromLeft (64).reduced (3));
        stopButton.setBounds (codeHeader.removeFromLeft (64).reduced (3));
        scriptEditor.setBounds (codePaneInner.reduced (0, 6));

        stateTabs.setBounds (area.removeFromTop (36));
        auto graphArea = workspace.reduced (0, 10);
        if (logVisible)
            logView.setBounds (graphArea.removeFromBottom (142).reduced (0, 8));

        graph.setBounds (graphArea);
    }

private:
    void configureSmallNumberEditor (juce::TextEditor& editor, int maxChars, const juce::String& allowedChars)
    {
        editor.setInputRestrictions (maxChars, allowedChars);
        editor.setJustification (juce::Justification::centred);
        editor.setMultiLine (false);
        editor.setSelectAllWhenFocused (true);
        editor.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff111318));
        editor.setColour (juce::TextEditor::textColourId, ink());
        editor.setColour (juce::TextEditor::outlineColourId, juce::Colour (0xff34414a));
        editor.setColour (juce::TextEditor::focusedOutlineColourId, accentA());
    }

    void updateCodeEditorFont()
    {
        scriptEditor.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), codeFontSize, juce::Font::plain)));
    }

    void updateCodeStats()
    {
        const auto text = codeDocument.getAllContent();
        const auto lineCount = codeDocument.getNumLines();
        const auto caret = scriptEditor.getCaretPos();

        codeStatsLabel.setText ("Ln " + juce::String (caret.getLineNumber() + 1)
                                + ", Col " + juce::String (caret.getIndexInLine() + 1)
                                + "  ·  " + juce::String (lineCount) + " lines"
                                + "  ·  " + juce::String (text.length()) + " chars",
                                juce::dontSendNotification);
    }

    void setCodeCheckStatus (const juce::String& text, juce::Colour colour)
    {
        codeCheckLabel.setText (text, juce::dontSendNotification);
        codeCheckLabel.setColour (juce::Label::textColourId, colour);
    }

    void checkSelectedLaneScript()
    {
        const auto checkId = host.checkScript (codeDocument.getAllContent(), getSclangPathOverride());
        if (checkId.isEmpty())
        {
            pendingCheckId.clear();
            setCodeCheckStatus ("Check failed: audio offline", accentC());
            return;
        }

        pendingCheckId = checkId;
        setCodeCheckStatus ("Checking...", accentA());
        pollCheckResult (checkId, 0);
    }

    void pollCheckResult (juce::String checkId, int attempt)
    {
        juce::Timer::callAfterDelay (120, [safeThis = juce::Component::SafePointer<MainComponent> (this), checkId, attempt]
        {
            if (safeThis == nullptr || safeThis->pendingCheckId != checkId)
                return;

            const auto result = safeThis->host.readCheckResult (checkId);
            if (result.isNotEmpty())
            {
                safeThis->handleCheckResultText (checkId, result);
                return;
            }

            if (attempt < 60)
                safeThis->pollCheckResult (checkId, attempt + 1);
            else
            {
                safeThis->pendingCheckId.clear();
                safeThis->setCodeCheckStatus ("Check timed out", accentC());
            }
        });
    }

    void handleCheckResultText (const juce::String& checkId, const juce::String& result)
    {
        if (pendingCheckId != checkId)
            return;

        pendingCheckId.clear();

        if (result.startsWith ("OK"))
        {
            setCodeCheckStatus ("OK", juce::Colour (0xff7bd88f));
            scriptEditor.deselectAll();
            return;
        }

        auto errorText = result.fromFirstOccurrenceOf ("ERROR", false, false).trim();
        if (errorText.isEmpty())
            errorText = "SuperCollider reported an error";

        auto line = extractErrorLineNumber (errorText);
        if (line <= 0)
            line = scriptEditor.getCaretPos().getLineNumber() + 1;

        highlightCodeLine (line);

        setCodeCheckStatus ("Error" + (line > 0 ? " line " + juce::String (line) : "") + ": " + errorText.upToFirstOccurrenceOf ("\n", false, false),
                            accentC());
    }

    void handleHostLogMessage (const juce::String& message)
    {
        handleSchedulerStateMessage (message);

        if (pendingCheckId.isEmpty())
            return;

        const auto okMarker = "MARKOV_CHECK_OK " + pendingCheckId;
        const auto errorMarker = "MARKOV_CHECK_ERROR " + pendingCheckId;

        if (message.contains (okMarker))
        {
            pendingCheckId.clear();
            setCodeCheckStatus ("OK", juce::Colour (0xff7bd88f));
            scriptEditor.deselectAll();
            return;
        }

        const auto errorIndex = message.indexOf (errorMarker);
        if (errorIndex >= 0)
        {
            auto errorText = message.substring (errorIndex + errorMarker.length()).trim();
            if (errorText.isEmpty())
                errorText = "SuperCollider reported an error";

            pendingCheckId.clear();
            auto line = extractErrorLineNumber (message);
            if (line <= 0)
                line = scriptEditor.getCaretPos().getLineNumber() + 1;

            highlightCodeLine (line);

            setCodeCheckStatus ("Error" + (line > 0 ? " line " + juce::String (line) : "") + ": " + errorText.upToFirstOccurrenceOf ("\n", false, false),
                                accentC());
        }
    }

    void handleSchedulerStateMessage (const juce::String& message)
    {
        const auto marker = "MARKOV_STATE ";
        const auto markerIndex = message.indexOf (marker);
        if (markerIndex < 0)
            return;

        auto payload = message.substring (markerIndex + juce::String (marker).length()).trim();
        auto parts = juce::StringArray::fromTokens (payload, " \t\r\n", "");
        if (parts.size() < 2 || parts[0] != machine.machineId)
            return;

        applySchedulerState (parts[0], parts[1].getIntValue());
    }

    void oscMessageReceived (const juce::OSCMessage& message) override
    {
        if (message.size() < 2 || ! message[0].isString())
            return;

        auto machineId = message[0].getString();
        auto stateIndex = -1;

        if (message[1].isInt32())
            stateIndex = message[1].getInt32();
        else if (message[1].isFloat32())
            stateIndex = static_cast<int> (message[1].getFloat32());
        else if (message[1].isString())
            stateIndex = message[1].getString().getIntValue();

        applySchedulerState (machineId, stateIndex);
    }

    void applySchedulerState (const juce::String& machineId, int stateIndex)
    {
        if (machineId != machine.machineId)
            return;

        if (stateIndex < 0 || stateIndex >= machine.getStateCount())
            return;

        machine.selectedState = stateIndex;
        machine.selectedLane = juce::jlimit (0, machine.getLaneCount (stateIndex) - 1, 0);
        refreshControls();
    }

    int extractErrorLineNumber (const juce::String& text) const
    {
        const auto lower = text.toLowerCase();
        auto index = lower.indexOf ("line ");
        if (index < 0)
            index = lower.indexOf ("line:");

        if (index < 0)
            return -1;

        index += 4;
        while (index < text.length() && ! juce::CharacterFunctions::isDigit (text[index]))
            ++index;

        juce::String digits;
        while (index < text.length() && juce::CharacterFunctions::isDigit (text[index]))
            digits << juce::String::charToString (text[index++]);

        return digits.getIntValue();
    }

    void highlightCodeLine (int oneBasedLine)
    {
        const auto line = juce::jlimit (0, codeDocument.getNumLines() - 1, oneBasedLine - 1);
        const juce::CodeDocument::Position start (codeDocument, line, 0);
        const juce::CodeDocument::Position end (codeDocument, line, codeDocument.getLine (line).length());
        scriptEditor.selectRegion (start, end);
        scriptEditor.scrollToLine (juce::jmax (0, line - 2));
    }

    juce::String tidyScriptText (const juce::String& source) const
    {
        juce::StringArray lines;
        lines.addLines (source);

        juce::StringArray tidied;
        int indent = 0;

        for (auto line : lines)
        {
            auto trimmed = line.trim();
            if (trimmed.isEmpty())
            {
                tidied.add ({});
                continue;
            }

            if (trimmed.startsWithChar ('}') || trimmed.startsWithChar (')') || trimmed.startsWithChar (']'))
                indent = juce::jmax (0, indent - 1);

            tidied.add (juce::String::repeatedString ("    ", indent) + trimmed);

            const auto opensBlock = trimmed.endsWithChar ('{')
                                  || trimmed.endsWithChar ('(')
                                  || trimmed.endsWithChar ('[')
                                  || trimmed.endsWith ("|");
            const auto closesBlock = trimmed.endsWithChar ('}')
                                  || trimmed.endsWithChar (')')
                                  || trimmed.endsWithChar (']');

            if (opensBlock && ! closesBlock)
                ++indent;
        }

        return tidied.joinIntoString ("\n") + "\n";
    }

    void tidySelectedLaneScript()
    {
        auto& lane = currentInspectorMachine().selectedLaneRef();
        const auto tidied = tidyScriptText (codeDocument.getAllContent());
        lane.script = tidied;
        lane.preparedBridge = -1;
        loadingCodeDocument = true;
        scriptEditor.loadContent (tidied);
        loadingCodeDocument = false;
        updateCodeStats();
        markMachineDirty();
    }

    void codeDocumentTextInserted (const juce::String&, int) override
    {
        codeDocumentChanged();
    }

    void codeDocumentTextDeleted (int, int) override
    {
        codeDocumentChanged();
    }

    void codeDocumentChanged()
    {
        if (loadingCodeDocument)
            return;

        currentInspectorMachine().selectedLaneRef().script = codeDocument.getAllContent();
        currentInspectorMachine().selectedLaneRef().preparedBridge = -1;
        if (pendingCheckId.isEmpty())
            setCodeCheckStatus ("Modified", mutedInk());
        updateCodeStats();
        markMachineDirty();
    }

    MachineModel& currentMachine() const
    {
        return *activeMachine;
    }

    MachineModel& currentInspectorMachine() const
    {
        return inspectedMachine != nullptr ? *inspectedMachine : *activeMachine;
    }

    MachineModel* selectedNestedMachine() const
    {
        return currentInspectorMachine().childMachine (currentInspectorMachine().selectedState);
    }

    juce::String makeBreadcrumb() const
    {
        juce::StringArray parts;
        parts.add ("Top FSM");
        addBreadcrumbParts (&machine, inspectedMachine, parts);
        parts.add (currentInspectorMachine().state (currentInspectorMachine().selectedState).name);
        return parts.joinIntoString (" / ");
    }

    bool addBreadcrumbParts (const MachineModel* model, const MachineModel* target, juce::StringArray& parts) const
    {
        if (model == target)
            return true;

        for (int i = 0; i < model->getStateCount(); ++i)
        {
            if (auto* child = model->childMachine (i))
            {
                parts.add (model->state (i).name + " FSM");
                if (addBreadcrumbParts (child, target, parts))
                    return true;
                parts.remove (parts.size() - 1);
            }
        }

        return false;
    }

    juce::String makeStateSummary() const
    {
        const auto& inspected = currentInspectorMachine();
        const auto& s = inspected.state (inspected.selectedState);
        const auto laneCount = static_cast<int> (s.lanes.size());
        auto activeText = (&inspected == activeMachine) ? "active" : "inspecting";
        const auto nestedText = inspected.hasChildMachine (inspected.selectedState) ? "nested FSM" : "no nested FSM";
        return s.name + "  ·  " + juce::String (laneCount) + (laneCount == 1 ? " track" : " tracks")
             + "  ·  " + juce::String (s.tempoBpm, 1) + " BPM"
             + "  ·  " + juce::String (s.beatsPerBar) + "/" + juce::String (s.beatUnit)
             + "  ·  " + activeText + "  ·  " + nestedText;
    }

    juce::String getSclangPathOverride() const
    {
        return {};
    }

    void commitNestedDivisionEditor()
    {
        if (auto* child = selectedNestedMachine())
        {
            child->parentDivision = juce::jlimit (1, 16, nestedDivisionEditor.getText().getIntValue());
            child->parentTickCounter = 0;
            markMachineDirty();
            refreshControls();
        }
    }

    void adjustNestedDivision (int delta)
    {
        if (auto* child = selectedNestedMachine())
        {
            child->parentDivision = juce::jlimit (1, 16, child->parentDivision + delta);
            child->parentTickCounter = 0;
            markMachineDirty();
            refreshControls();
        }
    }

    void commitStateTimingEditors()
    {
        auto& inspected = currentInspectorMachine();
        auto& state = inspected.state (inspected.selectedState);
        auto bpm = stateTempoEditor.getText().getDoubleValue();
        auto beats = stateMeterBeatsEditor.getText().getIntValue();
        auto unit = stateMeterUnitEditor.getText().getIntValue();

        state.tempoBpm = juce::jlimit (20.0, 320.0, bpm <= 0.0 ? state.tempoBpm : bpm);
        state.beatsPerBar = juce::jlimit (1, 32, beats <= 0 ? state.beatsPerBar : beats);
        state.beatUnit = juce::jlimit (1, 32, unit <= 0 ? state.beatUnit : unit);
        transportIntervalMs = getTransportIntervalMs();
        refreshControls();
    }

    void setActiveMachine (MachineModel& newMachine)
    {
        activeMachine = &newMachine;
        inspectedMachine = &newMachine;
        graph.setMachine (newMachine);
        graph.setInspectedMachine (&newMachine);
        rules.setMachine (newMachine);
        topStateCountEditor.setText (juce::String (machine.getStateCount()), false);
        refreshControls();
    }

    void commitTopLevelStateCountEditor()
    {
        setTopLevelStateCount (topStateCountEditor.getText().getIntValue());
    }

    void setTopLevelStateCount (int newCount)
    {
        newCount = juce::jlimit (1, maxStateCount, newCount);
        topStateCountEditor.setText (juce::String (newCount), false);

        if (newCount == machine.getStateCount())
            return;

        fsmRunning = false;
        stopTransport();
        host.stopAll (machine);
        runButton.setButtonText ("Run FSM");

        machineStack.clear();
        activeMachine = &machine;
        machine.setStateCount (newCount);
        graph.setMachine (machine);
        graph.setInspectedMachine (&machine);
        rules.setMachine (machine);
        markMachineDirty();
        refreshControls();
    }

    std::vector<LaneSnapshot> makeLaneSnapshots() const
    {
        std::vector<LaneSnapshot> lanes;
        collectLaneSnapshots (machine, lanes);

        return lanes;
    }

    void collectLaneSnapshots (const MachineModel& model, std::vector<LaneSnapshot>& lanes) const
    {
        for (const auto& state : model.states)
        {
            for (const auto& lane : state.lanes)
                lanes.push_back ({ lane.id, lane.name, lane.script, lane.volume });

            if (auto* child = model.childMachine (state.index))
                collectLaneSnapshots (*child, lanes);
        }
    }

    void markPreparedLanes (const std::vector<LaneSnapshot>& lanes, int bridge)
    {
        markPreparedLanesInMachine (machine, lanes, bridge);
    }

    void markPreparedLanesInMachine (MachineModel& model, const std::vector<LaneSnapshot>& lanes, int bridge)
    {
        for (const auto& snapshot : lanes)
        {
            for (auto& state : model.states)
            {
                for (auto& lane : state.lanes)
                    if (lane.id == snapshot.id)
                        lane.preparedBridge = bridge;

                if (auto* child = model.childMachine (state.index))
                    markPreparedLanesInMachine (*child, lanes, bridge);
            }
        }
    }

    void markMachineDirty()
    {
        machinePrepared = false;
    }

    void startPreparedRun()
    {
        runButton.setButtonText ("Pause");
        stopTransport();
        host.configureMachine (machine);
        host.runMachine (machine.selectedState, rateSlider.getValue());
        refreshControls();
    }

    void startPrepareJob (bool startAfterPrepare)
    {
        if (audioJobRunning.exchange (true))
            return;

        runButton.setButtonText (startAfterPrepare ? "Starting..." : "Run FSM");
        statusLabel.setText ("Booting audio", juce::dontSendNotification);

        auto lanes = makeLaneSnapshots();
        auto path = getSclangPathOverride();
        auto safeThis = juce::Component::SafePointer<MainComponent> (this);

        juce::Thread::launch ([safeThis, lanes, path, startAfterPrepare]
        {
            if (safeThis == nullptr)
                return;

            int preparedBridge = -1;
            for (const auto& lane : lanes)
            {
                if (safeThis == nullptr)
                    return;

                preparedBridge = safeThis->host.prepareData (lane, path);
                if (preparedBridge < 0)
                    break;
            }

            juce::MessageManager::callAsync ([safeThis, lanes, preparedBridge, startAfterPrepare]
            {
                if (safeThis == nullptr)
                    return;

                safeThis->audioJobRunning = false;

                if (preparedBridge >= 0)
                {
                    safeThis->markPreparedLanes (lanes, preparedBridge);
                    safeThis->host.configureMachine (safeThis->machine);
                    safeThis->machinePrepared = true;

                    if (startAfterPrepare && safeThis->fsmRunning)
                    {
                        safeThis->startPreparedRun();
                    }
                    else
                    {
                        safeThis->runButton.setButtonText ("Run FSM");
                    }
                }
                else
                {
                    safeThis->fsmRunning = false;
                    safeThis->runButton.setButtonText ("Run FSM");
                }

                safeThis->refreshControls();
            });
        });
    }

    void appendLog (const juce::String& message)
    {
        scLog << juce::Time::getCurrentTime().toString (true, true, true, true) << "  " << message << "\n";

        constexpr int maxLogChars = 24000;
        if (scLog.length() > maxLogChars)
            scLog = scLog.substring (scLog.length() - maxLogChars);

        logView.setText (scLog, juce::dontSendNotification);
        logView.moveCaretToEnd();
    }

    int getTransportIntervalMs() const
    {
        const auto& state = machine.state (machine.selectedState);
        const auto rate = juce::jmax (0.05, rateSlider.getValue());
        return juce::jlimit (80, 120000, static_cast<int> (state.secondsPerBar() * 1000.0 / rate));
    }

    double getTransportRateHz() const
    {
        return 1000.0 / static_cast<double> (getTransportIntervalMs());
    }

    int getTransportLookaheadMs() const
    {
        return juce::jlimit (30, 180, transportIntervalMs.load() / 3);
    }

    void startTransport()
    {
        stopTransport();
        transportShouldRun = true;
        ++transportCallbackGeneration;

        auto safeThis = juce::Component::SafePointer<MainComponent> (this);
        transportIntervalMs = getTransportIntervalMs();
        transportThread = std::thread ([safeThis]
        {
            auto nextTick = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds (safeThis != nullptr ? safeThis->transportIntervalMs.load() : 1000);

            while (safeThis != nullptr && safeThis->transportShouldRun)
            {
                const auto lookaheadMs = safeThis != nullptr ? safeThis->getTransportLookaheadMs() : 40;
                const auto scheduleTime = nextTick - std::chrono::milliseconds (lookaheadMs);

                {
                    std::unique_lock<std::mutex> lock (safeThis->transportMutex);
                    if (safeThis->transportCv.wait_until (lock, scheduleTime, [safeThis]
                    {
                        return safeThis == nullptr || ! safeThis->transportShouldRun.load();
                    }))
                    {
                        break;
                    }
                }

                if (safeThis == nullptr || ! safeThis->transportShouldRun)
                    break;

                auto result = std::make_shared<std::promise<int>>();
                auto future = result->get_future();
                const auto tickId = ++safeThis->transportCallbackGeneration;
                const auto targetTick = nextTick;

                juce::MessageManager::callAsync ([safeThis, result, tickId, targetTick]
                {
                    auto nextInterval = safeThis != nullptr ? safeThis->transportIntervalMs.load() : 1000;

                    if (safeThis != nullptr
                        && safeThis->transportShouldRun
                        && safeThis->transportCallbackGeneration.load() == tickId)
                    {
                        const auto remaining = std::chrono::duration_cast<std::chrono::microseconds> (targetTick - std::chrono::steady_clock::now()).count();
                        const auto delaySeconds = juce::jlimit (0.0, 0.5, static_cast<double> (remaining) / 1000000.0);
                        nextInterval = safeThis->advanceStateVisualOnly (delaySeconds);
                    }

                    result->set_value (nextInterval);
                });

                auto nextInterval = safeThis->transportIntervalMs.load();
                if (future.wait_for (std::chrono::milliseconds (750)) == std::future_status::ready)
                    nextInterval = future.get();

                if (safeThis != nullptr)
                    safeThis->transportIntervalMs = nextInterval;
                nextTick += std::chrono::milliseconds (nextInterval);

                const auto now = std::chrono::steady_clock::now();
                if (nextTick <= now)
                    nextTick = now + std::chrono::milliseconds (nextInterval);
            }
        });
    }

    void stopTransport()
    {
        transportShouldRun = false;
        ++transportCallbackGeneration;
        transportCv.notify_all();
        if (transportThread.joinable())
        {
            if (transportThread.get_id() != std::this_thread::get_id())
                transportThread.join();
            else
                transportThread.detach();
        }
    }

    void restartTransport()
    {
        if (! fsmRunning)
            return;

        startTransport();
    }

    void playState (int stateIndex)
    {
        auto& s = currentMachine().state (stateIndex);
        for (auto& lane : s.lanes)
            if (shouldPlayLane (s, lane))
                host.play (lane, getSclangPathOverride());
    }

    void prepareAllLanes()
    {
        for (auto& state : currentMachine().states)
            for (auto& lane : state.lanes)
                host.prepare (lane, getSclangPathOverride());
    }

    void stopState (int stateIndex)
    {
        for (auto& lane : currentMachine().state (stateIndex).lanes)
            host.stop (lane);
    }

    int advanceStateVisualOnly (double audioDelaySeconds = 0.0)
    {
        scheduledTransitionDelaySeconds = audioDelaySeconds;
        advanceMachineTree (machine);
        scheduledTransitionDelaySeconds = 0.0;
        const auto nextInterval = getTransportIntervalMs();
        transportIntervalMs = nextInterval;
        refreshControls();
        return nextInterval;
    }

    int chooseNextState (const MachineModel& model) const
    {
        std::vector<Rule> candidates;
        float total = 0.0f;

        for (const auto& rule : model.rules)
        {
            if (rule.from == model.selectedState)
            {
                candidates.push_back (rule);
                total += juce::jmax (0.0f, rule.weight);
            }
        }

        if (candidates.empty() || total <= 0.0f)
            return (model.selectedState + 1) % model.getStateCount();

        auto pick = juce::Random::getSystemRandom().nextFloat() * total;
        for (const auto& rule : candidates)
        {
            pick -= juce::jmax (0.0f, rule.weight);
            if (pick <= 0.0f)
                return rule.to;
        }

        return candidates.back().to;
    }

    void startMachine (MachineModel& model, int stateIndex)
    {
        model.entryState = juce::jlimit (0, model.getStateCount() - 1, stateIndex);
        model.stepsSinceEntry = 0;
        model.parentTickCounter = 0;
        model.oneShotComplete = false;
        model.latchedActive = true;
        enterState (model, model.entryState, true);
    }

    bool advanceMachineTree (MachineModel& model)
    {
        if (auto* child = model.childMachine (model.selectedState))
        {
            if (child->timingMode == NestedTimingMode::oneShot && ! child->oneShotComplete)
            {
                advanceSelectedChildMachine (model, true);
                ++model.stepsSinceEntry;
                return false;
            }
        }

        const auto nextState = chooseNextState (model);
        const auto parentIsHolding = nextState == model.selectedState;

        if (parentIsHolding)
            if (! advanceSelectedChildMachine (model, true))
                return false;

        enterState (model, nextState);
        ++model.stepsSinceEntry;

        return model.stepsSinceEntry > 0 && model.selectedState == model.entryState;
    }

    void enterState (MachineModel& model, int stateIndex, bool forceStart = false)
    {
        const auto previousState = model.selectedState;
        const auto changingState = previousState != stateIndex;

        if (! changingState && ! forceStart)
        {
            model.selectedLane = juce::jlimit (0, model.getLaneCount (model.selectedState) - 1, model.selectedLane);
            return;
        }

        std::vector<Lane*> lanesToStop;
        std::vector<Lane*> lanesToStart;

        if (changingState && previousState >= 0 && previousState < model.getStateCount())
        {
            for (auto& lane : model.state (previousState).lanes)
                lanesToStop.push_back (&lane);

            if (auto* child = model.childMachine (previousState))
                if (child->timingMode != NestedTimingMode::latch)
                    collectStopMachineRecursive (*child, lanesToStop);
        }

        model.selectedState = stateIndex;
        model.selectedLane = 0;

        auto& state = model.state (stateIndex);
        for (auto& lane : state.lanes)
            if (shouldPlayLane (state, lane))
                lanesToStart.push_back (&lane);

        host.transition (lanesToStop, lanesToStart, getSclangPathOverride(), musicalReleaseSeconds, scheduledTransitionDelaySeconds);

        if (auto* child = model.childMachine (stateIndex))
            startChildMachineForParentState (*child);
    }

    void startChildMachineForParentState (MachineModel& child)
    {
        child.parentTickCounter = 0;
        child.oneShotComplete = false;

        if (child.timingMode != NestedTimingMode::latch || ! child.latchedActive)
            startMachine (child, child.entryState);
        else if (child.timingMode == NestedTimingMode::latch)
            child.latchedActive = true;
    }

    bool advanceSelectedChildMachine (MachineModel& parent, bool parentIsHolding)
    {
        auto* child = parent.childMachine (parent.selectedState);
        if (child == nullptr)
            return true;

        ++child->parentTickCounter;
        if (child->parentTickCounter < child->parentDivision)
            return true;

        child->parentTickCounter = 0;

        switch (child->timingMode)
        {
            case NestedTimingMode::followParent:
                advanceMachineTree (*child);
                return true;

            case NestedTimingMode::freeRun:
                advanceMachineTree (*child);
                return true;

            case NestedTimingMode::oneShot:
                if (! child->oneShotComplete)
                {
                    child->oneShotComplete = advanceMachineTree (*child);
                    if (child->oneShotComplete)
                        stopMachineRecursive (*child);
                }
                return child->oneShotComplete || parentIsHolding;

            case NestedTimingMode::latch:
                advanceMachineTree (*child);
                return true;
        }

        return true;
    }

    void stopMachineRecursive (MachineModel& model)
    {
        model.latchedActive = false;
        model.oneShotComplete = false;
        for (auto& state : model.states)
        {
            for (auto& lane : state.lanes)
                host.stop (lane);

            if (auto* child = model.childMachine (state.index))
                stopMachineRecursive (*child);
        }
    }

    void collectStopMachineRecursive (MachineModel& model, std::vector<Lane*>& lanesToStop)
    {
        model.latchedActive = false;
        model.oneShotComplete = false;

        for (auto& state : model.states)
        {
            for (auto& lane : state.lanes)
                lanesToStop.push_back (&lane);

            if (auto* child = model.childMachine (state.index))
                collectStopMachineRecursive (*child, lanesToStop);
        }
    }

    bool shouldPlayLane (const State& state, const Lane& lane) const
    {
        if (! lane.enabled || lane.muted)
            return false;

        const auto anySolo = std::any_of (state.lanes.begin(), state.lanes.end(), [] (const Lane& l)
        {
            return l.enabled && l.solo;
        });

        return ! anySolo || lane.solo;
    }

    void refreshControls()
    {
        refreshStateTabs();
        refreshTrackList();
        rules.setMachine (currentInspectorMachine());

        if (! scriptEditor.hasKeyboardFocus (true) || codeDocument.getAllContent() != currentInspectorMachine().selectedLaneRef().script)
        {
            loadingCodeDocument = true;
            scriptEditor.loadContent (currentInspectorMachine().selectedLaneRef().script);
            loadingCodeDocument = false;
        }
        updateCodeStats();
        trackNameEditor.setText (currentInspectorMachine().selectedLaneRef().name, false);
        breadcrumbLabel.setText (makeBreadcrumb(), juce::dontSendNotification);
        stateSummaryLabel.setText (makeStateSummary(), juce::dontSendNotification);
        const auto& inspectedState = currentInspectorMachine().state (currentInspectorMachine().selectedState);
        stateTempoEditor.setText (juce::String (inspectedState.tempoBpm, 1), false);
        stateMeterBeatsEditor.setText (juce::String (inspectedState.beatsPerBar), false);
        stateMeterUnitEditor.setText (juce::String (inspectedState.beatUnit), false);
        if (auto* child = selectedNestedMachine())
        {
            nestedModeBox.setEnabled (true);
            nestedDivisionMinus.setEnabled (true);
            nestedDivisionEditor.setEnabled (true);
            nestedDivisionPlus.setEnabled (true);
            nestedModeBox.setSelectedItemIndex (static_cast<int> (child->timingMode), juce::dontSendNotification);
            nestedDivisionEditor.setText (juce::String (child->parentDivision), false);
        }
        else
        {
            nestedModeBox.setEnabled (false);
            nestedDivisionMinus.setEnabled (false);
            nestedDivisionEditor.setEnabled (false);
            nestedDivisionPlus.setEnabled (false);
            nestedModeBox.setSelectedItemIndex (0, juce::dontSendNotification);
            nestedDivisionEditor.setText ("-", false);
        }
        topStateCountEditor.setText (juce::String (machine.getStateCount()), false);
        playButton.setColour (juce::TextButton::buttonColourId, currentInspectorMachine().selectedLaneRef().playing ? accentA().darker (0.2f) : accentB().darker (0.35f));
        moveLaneUpButton.setEnabled (currentInspectorMachine().selectedLane > 0);
        moveLaneDownButton.setEnabled (currentInspectorMachine().selectedLane < currentInspectorMachine().getLaneCount (currentInspectorMachine().selectedState) - 1);
        const auto hasChild = currentInspectorMachine().hasChildMachine (currentInspectorMachine().selectedState);
        addChildMachineButton.setEnabled (! hasChild);
        enterChildMachineButton.setEnabled (hasChild);
        exitChildMachineButton.setEnabled (! machineStack.empty());
        graph.repaint();
        rules.repaint();
        graph.setInspectedMachine (&currentInspectorMachine());
    }

    void refreshStateTabs()
    {
        juce::StringArray names;
        for (int i = 0; i < currentMachine().getStateCount(); ++i)
            names.add (currentMachine().state (i).name);

        stateTabs.setItems (names, currentMachine().selectedState);
    }

    void refreshTrackList()
    {
        auto& inspected = currentInspectorMachine();
        auto& s = inspected.state (inspected.selectedState);
        trackList.setState (s, inspected.selectedLane);
    }

    MachineModel machine;
    MachineModel* activeMachine = &machine;
    MachineModel* inspectedMachine = &machine;
    std::vector<MachineModel*> machineStack;
    SuperColliderHost host;
    GraphComponent graph;
    juce::TextButton graphFitButton;
    juce::TextButton graphLayoutButton;
    RuleListComponent rules;
    PaneDivider graphBottomDivider { PaneDivider::Orientation::horizontal };
    PaneDivider rulesTracksDivider;
    PaneDivider tracksCodeDivider;

    juce::Label title;
    ClickableLabel statusLabel;
    juce::TextButton logButton;
    juce::TextButton panicButton;
    juce::Label topStateCountLabel;
    juce::TextButton topStateCountMinus;
    juce::TextEditor topStateCountEditor;
    juce::TextButton topStateCountPlus;
    juce::TextButton runButton;
    juce::TextButton stepButton;
    juce::TextButton stopAllButton;
    juce::Slider rateSlider;
    PillBar stateTabs;
    juce::Label breadcrumbLabel;
    juce::Label stateSummaryLabel;
    juce::Label stateTempoLabel;
    juce::TextEditor stateTempoEditor;
    juce::Label stateMeterLabel;
    juce::TextEditor stateMeterBeatsEditor;
    juce::Label stateMeterSlashLabel;
    juce::TextEditor stateMeterUnitEditor;
    juce::Label nestedTimingLabel;
    juce::ComboBox nestedModeBox;
    juce::Label nestedDivisionLabel;
    juce::TextButton nestedDivisionMinus;
    juce::TextEditor nestedDivisionEditor;
    juce::TextButton nestedDivisionPlus;
    juce::Label trackPaneTitle;
    juce::TextEditor trackNameEditor;
    TrackListComponent trackList;
    juce::Label codePaneTitle;
    juce::Label codeStatsLabel;
    juce::Label codeCheckLabel;
    juce::TextButton checkCodeButton;
    juce::TextButton codeFontDownButton;
    juce::TextButton codeFontUpButton;
    juce::TextButton tidyCodeButton;
    juce::TextButton expandCodeButton;
    SuperColliderTokeniser scTokeniser;
    juce::CodeDocument codeDocument;
    SuperColliderCodeEditor scriptEditor;
    juce::TextButton addLaneButton;
    juce::TextButton removeLaneButton;
    juce::TextButton moveLaneUpButton;
    juce::TextButton moveLaneDownButton;
    juce::TextButton addChildMachineButton;
    juce::TextButton enterChildMachineButton;
    juce::TextButton exitChildMachineButton;
    juce::TextButton playButton;
    juce::TextButton stopButton;
    juce::TextEditor logView;
    juce::String scLog;
    bool logVisible = false;
    bool codeExpanded = false;
    bool fsmRunning = false;
    bool machinePrepared = false;
    int rulesPaneWidth = 500;
    int tracksPaneWidth = 210;
    int bottomPaneHeight = 250;
    int dividerDragStartRulesWidth = 500;
    int dividerDragStartTracksWidth = 210;
    int dividerDragStartBottomHeight = 250;
    bool rulesPaneUserSized = false;
    bool tracksPaneUserSized = false;
    bool bottomPaneUserSized = false;
    std::atomic<bool> audioJobRunning { false };
    std::atomic<bool> transportShouldRun { false };
    std::atomic<int> transportIntervalMs { 2000 };
    std::atomic<int> transportCallbackGeneration { 0 };
    double scheduledTransitionDelaySeconds = 0.0;
    float codeFontSize = 14.0f;
    std::mutex transportMutex;
    std::condition_variable transportCv;
    std::thread transportThread;
    bool loadingCodeDocument = false;
    juce::String pendingCheckId;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

class MarkovApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "Markov FSM"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise (const juce::String&) override
    {
        mainWindow = std::make_unique<MainWindow> (getApplicationName());
    }

    void shutdown() override
    {
        mainWindow = nullptr;
    }

    void systemRequestedQuit() override
    {
        quit();
    }

private:
    class MainWindow final : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (juce::String name)
            : DocumentWindow (std::move (name), backgroundTop(), DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent(), true);
            setResizable (true, true);
            if (auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
                setBounds (display->userArea);
            else
                centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }
    };

    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (MarkovApplication)
