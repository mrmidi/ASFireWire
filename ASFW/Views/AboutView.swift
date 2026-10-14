import SwiftUI

struct AboutView: View {
    private let projectURL = URL(string: "https://www.asfw.net")!
    private let supportURL = URL(string: "https://www.asfw.net/support/")!
    private let sponsorsURL = URL(string: "https://github.com/sponsors/mrmidi")!
    private let coffeeURL = URL(string: "https://buymeacoffee.com/mistermidi")!
    private let recognitionEmail = URL(string: "mailto:me@mrmidi.net?subject=ASFireWire%20support%20recognition")!

    private var appVersion: String {
        let info = Bundle.main.infoDictionary ?? [:]
        let version = info["CFBundleShortVersionString"] as? String ?? "Unknown version"
        let build = info["CFBundleVersion"] as? String
        return build.map { "Version \(version) (\($0))" } ?? "Version \(version)"
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 22) {
                hero
                supportSection
                contributorsSection
            }
            .frame(maxWidth: 850, alignment: .leading)
            .padding(28)
            .frame(maxWidth: .infinity, alignment: .top)
        }
        .navigationTitle("About ASFireWire")
    }

    private var hero: some View {
        HStack(alignment: .center, spacing: 20) {
            Image(systemName: "hifispeaker.2.fill")
                .font(.system(size: 34, weight: .medium))
                .foregroundStyle(.white)
                .frame(width: 76, height: 76)
                .background(
                    LinearGradient(colors: [.blue, .cyan.opacity(0.8)], startPoint: .topLeading, endPoint: .bottomTrailing),
                    in: RoundedRectangle(cornerRadius: 20)
                )
                .accessibilityHidden(true)

            VStack(alignment: .leading, spacing: 6) {
                Text("ASFireWire")
                    .font(.largeTitle.bold())
                Text("FireWire audio, video, and storage for modern macOS.")
                    .font(.title3)
                    .foregroundStyle(.secondary)
                Text(appVersion)
                    .font(.callout.monospacedDigit())
                    .foregroundStyle(.tertiary)
                Text("Created and maintained by Alexander Shabelnikov")
                    .font(.callout)
                    .foregroundStyle(.secondary)
                Link("asfw.net", destination: projectURL)
                    .font(.callout)
            }
            Spacer(minLength: 0)
        }
        .padding(24)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 22))
    }

    private var supportSection: some View {
        sectionCard(title: "Support the person behind it", symbol: "heart") {
            Text("I built ASFireWire so people could keep using their favorite gear, or find good professional hardware affordably. I chose to keep it free and open source so more people could benefit.")
            Text("Financial support helps cover my everyday living expenses and development tools, making it possible for me to continue developing and maintaining ASFireWire.")
                .foregroundStyle(.secondary)

            ViewThatFits(in: .horizontal) {
                HStack(spacing: 12) {
                    fundingLink("GitHub Sponsors", symbol: "heart.fill", destination: sponsorsURL, prominent: true)
                    fundingLink("Buy Me a Coffee", symbol: "cup.and.saucer.fill", destination: coffeeURL, prominent: false)
                }
                VStack(spacing: 10) {
                    fundingLink("GitHub Sponsors", symbol: "heart.fill", destination: sponsorsURL, prominent: true)
                    fundingLink("Buy Me a Coffee", symbol: "cup.and.saucer.fill", destination: coffeeURL, prominent: false)
                }
            }

            Text("Recognition is optional. If you support the project and would like your preferred name and an optional link included here, send me a note. You are equally welcome to remain anonymous.")
                .font(.callout)
                .foregroundStyle(.secondary)

            ViewThatFits(in: .horizontal) {
                HStack(spacing: 16) {
                    recognitionLink
                    Link("Read more about support", destination: supportURL)
                }
                VStack(alignment: .leading, spacing: 10) {
                    recognitionLink
                    Link("Read more about support", destination: supportURL)
                }
            }
        }
    }

    private var contributorsSection: some View {
        sectionCard(title: "Technical contributors", symbol: "person.2") {
            Text("Code, protocol work, and sustained testing on real hardware have helped shape ASFireWire. This selected list is incomplete; if I missed you, please get in touch.")
                .foregroundStyle(.secondary)

            VStack(alignment: .leading, spacing: 12) {
                contributor("@alicankaralar", detail: "Initial Midas Venice F32 DICE work", url: "https://github.com/alicankaralar")
                contributor("@deweydb", detail: "Initial MOTU V2 work", url: "https://github.com/deweydb")
                contributor("@gdemontalivet", detail: "DV capture hardening", url: "https://github.com/gdemontalivet")
                contributor("@gly11", detail: "Initial SBP-2 work", url: "https://github.com/gly11")
                contributor("@hoffmabc", detail: "Initial DV capture support", url: "https://github.com/hoffmabc")
                contributor("Joe Mountain Music", detail: "Venice F24 testing and Pro Tools sessions", url: "https://www.joemountainmusic.com")
                contributor("@KinoLab07", detail: "Avid Mbox Pro (3rd generation) DICE work", url: "https://github.com/KinoLab07")
                contributor("@klochowicz", detail: "Initial PreSonus work", url: "https://github.com/klochowicz")
                contributor("@mhellevang", detail: "SBP-2/SCSI stack and storage-class support", url: "https://github.com/mhellevang")
            }
            .padding(.top, 4)

            HStack(spacing: 14) {
                Link("Code contributors", destination: URL(string: "https://github.com/mrmidi/ASFireWire/graphs/contributors")!)
                Link("Hardware-test credits", destination: URL(string: "https://github.com/mrmidi/ASFireWire/wiki/Device-Compatibility")!)
            }
            .font(.callout)
            .padding(.top, 4)
        }
    }

    private func sectionCard<Content: View>(
        title: String,
        symbol: String,
        @ViewBuilder content: () -> Content
    ) -> some View {
        VStack(alignment: .leading, spacing: 16) {
            Label(title, systemImage: symbol)
                .font(.title2.bold())
            content()
        }
        .padding(22)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 20))
    }

    @ViewBuilder
    private func fundingLink(_ title: String, symbol: String, destination: URL, prominent: Bool) -> some View {
        if prominent {
            Link(destination: destination) {
                fundingLabel(title, symbol: symbol)
            }
            .buttonStyle(.borderedProminent)
        } else {
            Link(destination: destination) {
                fundingLabel(title, symbol: symbol)
            }
            .buttonStyle(.bordered)
        }
    }

    private func fundingLabel(_ title: String, symbol: String) -> some View {
        Label(title, systemImage: symbol)
            .frame(maxWidth: .infinity, minHeight: 42)
            .contentShape(RoundedRectangle(cornerRadius: 10))
    }

    private func contributor(_ name: String, detail: String, url: String) -> some View {
        ViewThatFits(in: .horizontal) {
            HStack(alignment: .firstTextBaseline, spacing: 12) {
                Link(name, destination: URL(string: url)!)
                    .font(.body.weight(.semibold))
                    .frame(minWidth: 180, alignment: .leading)
                Text(detail)
                    .foregroundStyle(.secondary)
            }
            VStack(alignment: .leading, spacing: 3) {
                Link(name, destination: URL(string: url)!).font(.body.weight(.semibold))
                Text(detail).foregroundStyle(.secondary)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private var recognitionLink: some View {
        Link(destination: recognitionEmail) {
            Label("Let me know your preference", systemImage: "envelope")
        }
        .accessibilityHint("Opens an email to request optional supporter recognition")
    }
}
