package dev.codeharbor.mobile;

/** Standalone host test: compile with the production NavigationDecision.java. */
public final class NavigationDecisionTest {
    private static final String MARKER = "about:blank#ch-policy-769be557-8e3b-4dd3-a2f5-e96ed9dfcd27";

    private static void expect(String url, boolean setup, NavigationDecision.Result expected) {
        NavigationDecision.Result actual = NavigationDecision.decide(url, MARKER, setup);
        if (actual != expected)
            throw new AssertionError("Unexpected decision for " + url + ": " + actual + " != " + expected);
    }

    public static void main(String[] arguments) {
        String[] remote = {
            "https://example.com/", "http://example.com/path?q=1#anchor",
            "HTTPS://example.com/", "https://127.0.0.1:8443/",
            "http://[::1]:8080/", "https://xn--bcher-kva.example/"
        };
        for (String url : remote) {
            expect(url, false, NavigationDecision.Result.REMOTE);
            expect(url, true, NavigationDecision.Result.REMOTE);
        }
        String[] blocked = {
            null, "", "about:blank", "about:blank#not-owned", "file:///etc/passwd",
            "content://dev.codeharbor.mobile/private", "data:text/html,hello",
            "javascript:alert(1)", "intent://example/#Intent;scheme=https;end",
            "mailto:test@example.com", "tel:+1234", "geo:1,2", "custom:payload",
            "ftp://example.com/", "https:", "https:example.com", "https:///example.com",
            "https://", "https://example.com:99999/", "https://example.com\\@evil.com/",
            " https://example.com/", "https://example.com/\nintent:bad",
            "https://example.com/%zz", "//example.com/", "/relative"
        };
        for (String url : blocked) {
            expect(url, false, NavigationDecision.Result.BLOCK);
            expect(url, true, NavigationDecision.Result.BLOCK);
        }
        expect(MARKER, true, NavigationDecision.Result.BOOTSTRAP);
        expect(MARKER, false, NavigationDecision.Result.BLOCK);
        if (NavigationDecision.decide(MARKER, null, true) != NavigationDecision.Result.BLOCK)
            throw new AssertionError("Missing marker must never permit bootstrap");
        String[] inline = {
            "data:image/png;base64,aGVsbG8=", "blob:https://example.com/known-object",
            "DATA:text/plain,inline"
        };
        for (String url : inline) {
            if (!NavigationDecision.allowInlineResource(url, false))
                throw new AssertionError("Inline resource should remain available: " + url);
            if (NavigationDecision.allowInlineResource(url, true))
                throw new AssertionError("Inline scheme must not permit a main-frame request: " + url);
            expect(url, false, NavigationDecision.Result.BLOCK);
        }
        String[] deniedResources = {
            null, "", "file:///private/secret", "content://provider/private",
            "intent://external", "custom://external", "ftp://example.com/file",
            "data:broken%zz", "blob:broken%zz"
        };
        for (String url : deniedResources) {
            if (NavigationDecision.allowInlineResource(url, false)
                    || NavigationDecision.allowInlineResource(url, true))
                throw new AssertionError("Denied resource escaped policy: " + url);
        }
        System.out.println("NavigationDecisionTest: all decisions passed");
    }
}
