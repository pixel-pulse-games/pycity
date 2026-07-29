async function fetchLatestRelease() {
    const container = document.getElementById('latest-release-info');
    if (!container) return;

    try {
        const response = await fetch('https://api.github.com/repos/pycity-project/pycity-project.github.io/releases/latest', {
            headers: { 'User-Agent': 'PyCity-Website-Updater' }
        });

        if (response.status === 403) throw new Error('RateLimitExceeded');
        if (!response.ok) throw new Error('Failed to fetch release metadata');

        const data = await response.json();
        let bodyText = data.body || 'No release notes provided.';

        // --- Simple Markdown to HTML Parser ---
        bodyText = bodyText
            .replace(/^### (.*$)/gim, '<h4 style="color: var(--text-color); margin: 15px 0 5px 0;">$1</h4>') // ### H3 -> H4
            .replace(/^## (.*$)/gim, '<h3 style="color: var(--text-color); margin: 15px 0 5px 0;">$1</h3>')   // ## H2 -> H3
            .replace(/\*\*(.*?)\*\*/g, '<strong>$1</strong>')                                                 // **Bold**
            .replace(/^\s*-\s*(.*$)/gim, '<li style="margin-left: 15px; margin-bottom: 4px;">$1</li>')       // - Lists
            .replace(/\r\n|\n/g, '<br>');                                                                    // Line breaks

        container.innerHTML = `
            <div style="margin: 20px 0; padding: 15px; border: 1px solid var(--border-color); border-radius: 6px; background-color: var(--btn-bg); color: var(--text-color);">
                <strong style="font-size: 16px;">Latest Version: ${data.name || data.tag_name}</strong>
                <div style="margin-top: 12px; font-size: 14px; line-height: 1.6;">${bodyText}</div>
            </div>
        `;
    } catch (error) {
        console.error(error);
        if (error.message === 'RateLimitExceeded') {
            container.innerHTML = `<p style="color: var(--text-color);">API limit reached. View updates on <a href="https://github.com/pixel-pulse-games/pycity/releases" target="_blank" style="color: #58a6ff; text-decoration: underline;">GitHub Releases</a>.</p>`;
        } else {
            container.innerHTML = `<p style="color: red;">Could not load version notes automatically.</p>`;
        }
    }
}

document.addEventListener('DOMContentLoaded', fetchLatestRelease);
