package dev.codeharbor.mobile;

import java.net.URI;
import java.net.URISyntaxException;

/** No Android dependencies: the same decision is exercised by the host test. */
public final class NavigationDecision {
    public enum Result { BLOCK, BOOTSTRAP, REMOTE }

    private NavigationDecision() {}

    public static Result decide(String url, String marker, boolean bootstrapping) {
        if (url == null || url.isEmpty())
            return Result.BLOCK;
        if (bootstrapping && marker != null && marker.equals(url))
            return Result.BOOTSTRAP;
        try {
            URI parsed = new URI(url);
            String scheme = parsed.getScheme();
            if (!("http".equalsIgnoreCase(scheme) || "https".equalsIgnoreCase(scheme))
                    || parsed.isOpaque() || parsed.getHost() == null
                    || parsed.getHost().isEmpty() || parsed.getPort() > 65535)
                return Result.BLOCK;
            return Result.REMOTE;
        } catch (URISyntaxException exception) {
            return Result.BLOCK;
        }
    }

    // This is resource policy, never navigation policy. Android's modern
    // interception callback distinguishes main-frame requests, not frame/image
    // types; all shouldOverrideUrlLoading callbacks remain HTTP(S)-only.
    public static boolean allowInlineResource(String url, boolean mainFrame) {
        if (mainFrame || url == null)
            return false;
        try {
            String scheme = new URI(url).getScheme();
            return "data".equalsIgnoreCase(scheme) || "blob".equalsIgnoreCase(scheme);
        } catch (URISyntaxException exception) {
            return false;
        }
    }
}
