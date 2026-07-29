async function fetchLatestRelease() {
    const container = document.getElementById('latest-release-info');
    if (!container) return;

    try {
        // Includes a custom User-Agent string to comply with GitHub API guidelines
        const response = await fetch('https://api.github.com/repos/pycity-project/pycity-project.github.io/releases/latest', {
            headers: {
                'User-Agent': 'PyCity-Website-Updater'
            }
        });

        // Specific error handling for hitting GitHub API rate limits (HTTP 403)
        if (response.status === 403) {
            throw new Error('RateLimitExceeded');
        }

        if (!response.ok) {
            throw new Error('Failed to fetch release metadata');
        }

        const data = await response.json();
        const cleanBody = data.body ? data.body.replace(/\r\n|\n/g, '<br>') : 'No release notes provided.';

        container.innerHTML = `
            <div style="margin: 20px 0; padding: 15px; border: 1px solid var(--border-color); border-radius: 6px; background-color: var(--btn-bg);">
                <strong style="color: var(--text-color); font-size: 16px;">Latest Version: ${data.name || data.tag_name}</strong>
                <p style="color: var(--text-color); margin-top: 8px; font-size: 14px; line-height: 1.5;">${cleanBody}</p>
            </div>
        `;
    } catch (error) {
        console.error(error);
        
        // Displays a user-friendly manual download link if the API fails
        if (error.message === 'RateLimitExceeded') {
            container.innerHTML = `
                <p style="color: var(--text-color);">
                    API limit reached. Please view updates directly on the 
                    <a href="https://github.com" target="_blank" style="color: #58a6ff; text-decoration: underline;">GitHub Releases Page</a>.
                </p>`;
        } else {
            container.innerHTML = `<p style="color: red;">Could not load version notes automatically.</p>`;
        }
    }
}

document.addEventListener('DOMContentLoaded', fetchLatestRelease);
