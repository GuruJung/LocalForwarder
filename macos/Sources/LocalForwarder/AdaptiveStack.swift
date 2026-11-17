import AppKit

/// Reflows a complete row rather than letting AppKit detach low-priority controls.
final class AdaptiveStack: NSStackView {
    private var verticalWidths: [NSLayoutConstraint] = []
    private var horizontalConstraints: [NSLayoutConstraint] = []
    private var compact = false

    init(views: [NSView], spacing: CGFloat = 10, horizontalConstraints: [NSLayoutConstraint] = []) {
        super.init(frame: .zero)
        self.spacing = spacing; self.horizontalConstraints = horizontalConstraints
        orientation = .horizontal; alignment = .centerY; detachesHiddenViews = false
        for view in views {
            addArrangedSubview(view); setVisibilityPriority(.mustHold, for: view)
            verticalWidths.append(view.widthAnchor.constraint(equalTo: widthAnchor))
        }
        NSLayoutConstraint.activate(horizontalConstraints)
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }
    func reflow(compact: Bool) {
        guard self.compact != compact else { return }
        self.compact = compact
        NSLayoutConstraint.deactivate(compact ? horizontalConstraints : verticalWidths)
        orientation = compact ? .vertical : .horizontal
        alignment = compact ? .leading : .centerY
        NSLayoutConstraint.activate(compact ? verticalWidths : horizontalConstraints)
    }
}
