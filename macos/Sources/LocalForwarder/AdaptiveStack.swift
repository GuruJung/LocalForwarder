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
    var minimumHorizontalWidth: CGFloat {
        arrangedSubviews.reduce(spacing * CGFloat(max(0, arrangedSubviews.count - 1))) { width, view in
            if let row = view as? AdaptiveStack { return width + row.minimumHorizontalWidth }
            if let field = view as? NSTextField, field.isEditable { return width + 80 }
            return width + max(0, view.intrinsicContentSize.width)
        }
    }
    func reflow(compact: Bool) {
        guard self.compact != compact else { return }
        self.compact = compact
        NSLayoutConstraint.deactivate(compact ? horizontalConstraints : verticalWidths)
        orientation = compact ? .vertical : .horizontal
        alignment = compact ? .leading : .centerY
        NSLayoutConstraint.activate(compact ? verticalWidths : horizontalConstraints)
    }
}
