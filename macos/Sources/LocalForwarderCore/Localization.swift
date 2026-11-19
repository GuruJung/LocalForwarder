import Foundation

/// App-authored messages retain identity and arguments; external diagnostics remain opaque.
public indirect enum LocalizedMessage: Equatable {
    case raw(String)
    case translated(String, [String: String])
    case joined([LocalizedMessage])
    public static func key(_ key: String, _ arguments: [String: String] = [:]) -> Self { .translated(key, arguments) }
    public static func error(_ error: Error) -> Self {
        if let error = error as? AppError { return error.localizedMessage }
        if let error = error as? LoginItemError { return error.localizedMessage }
        return .raw(error.localizedDescription)
    }
    public var rendered: String { rendered(language: L10n.language) }
    public func rendered(language: String) -> String {
        switch self {
        case .raw(let value): return value
        case .translated(let key, let arguments): return L10n.text(key, arguments, language: language)
        case .joined(let parts): return parts.map { $0.rendered(language: language) }.joined()
        }
    }
    public static func + (lhs: Self, rhs: Self) -> Self { .joined([lhs, rhs]) }
}

public enum L10n {
    public static let languages = ["zh-Hans", "zh-Hant", "en", "de", "ja", "ko", "es"]
    public static let languageNames = ["Chinese (Simplified) — 简体中文", "Chinese (Traditional) — 繁體中文", "English", "German — Deutsch", "Japanese — 日本語", "Korean — 한국어", "Spanish — Español"]
    private static let lock = NSLock()
    private static var selected = "auto"
    private static var resolved = "en"
    public static var selection: String { lock.lock(); defer { lock.unlock() }; return selected }
    public static var language: String { lock.lock(); defer { lock.unlock() }; return resolved }
    public static func normalizedSelection(_ value: String?) -> String {
        guard let value, value == "auto" || languages.contains(value) else { return "auto" }
        return value
    }
    public static func match(_ tag: String) -> String? {
        let parts = tag.replacingOccurrences(of: "_", with: "-").lowercased().split(separator: "-").map(String.init)
        guard let primary = parts.first else { return nil }
        if ["en", "ko", "ja", "es", "de"].contains(primary) { return primary }
        guard primary == "zh" else { return nil }
        if parts.contains("hans") { return "zh-Hans" }
        if parts.contains("hant") { return "zh-Hant" }
        return parts.contains(where: { ["tw", "hk", "mo"].contains($0) }) ? "zh-Hant" : "zh-Hans"
    }
    public static func resolve(_ selection: String?, preferred: [String]) -> String {
        let choice = normalizedSelection(selection)
        return choice == "auto" ? preferred.compactMap(match).first ?? "en" : choice
    }
    /// Explicit context injection keeps tests and the authentication helper independent of user preferences.
    public static func configure(selection: String?, preferred: [String] = Locale.preferredLanguages) {
        let choice = normalizedSelection(selection), language = resolve(selection, preferred: preferred)
        lock.lock(); selected = choice; resolved = language; lock.unlock()
    }
    public static func text(_ key: String, _ arguments: [String: String] = [:], language: String? = nil) -> String {
        let language = language ?? self.language
        let translated = GeneratedTranslations.values[language]?[key]
        let template = translated.flatMap { $0.isEmpty ? nil : $0 } ?? GeneratedTranslations.values["en"]?[key] ?? key
        return format(template, arguments)
    }
    /// Scan the template once. Argument text, including braces and percent signs, is never reinterpreted.
    public static func format(_ template: String, _ arguments: [String: String]) -> String {
        var result = "", position = template.startIndex
        while position < template.endIndex {
            if template[position] == "{", let end = template[position...].firstIndex(of: "}") {
                let name = String(template[template.index(after: position)..<end])
                if let value = arguments[name] { result += value; position = template.index(after: end); continue }
            }
            result.append(template[position]); position = template.index(after: position)
        }
        return result
    }
    public static var automaticTitle: String {
        text("mac.language.automatic")
    }
}

/// Isolated adapters make failed saves testable without modifying the user's preferences.
public struct LanguagePreferences {
    public var load: () -> String?
    public var save: (String) throws -> Void
    public var preferred: () -> [String]
    public init(load: @escaping () -> String?, save: @escaping (String) throws -> Void,
                preferred: @escaping () -> [String] = { Locale.preferredLanguages }) {
        self.load = load; self.save = save; self.preferred = preferred
    }
    public func start() { L10n.configure(selection: load(), preferred: preferred()) }
    public func select(_ selection: String) throws {
        let normalized = L10n.normalizedSelection(selection)
        try save(normalized)
        L10n.configure(selection: normalized, preferred: preferred())
    }
    public static var system: Self {
        let domain = "com.gurujung.LocalForwarder", key = "displayLanguage"
        // Foundation rejects a named suite matching the running app's bundle identifier.
        let defaults = Bundle.main.bundleIdentifier == domain ? UserDefaults.standard : UserDefaults(suiteName: domain)
        return Self(load: { defaults?.string(forKey: key) }, save: { value in
            guard let defaults else { throw AppError(.key("mac.language.save.failed")) }
            let previous = defaults.object(forKey: key)
            defaults.set(value, forKey: key)
            guard defaults.synchronize() else {
                if let previous { defaults.set(previous, forKey: key) } else { defaults.removeObject(forKey: key) }
                _ = defaults.synchronize()
                throw AppError(.key("mac.language.save.failed"))
            }
        })
    }
}
