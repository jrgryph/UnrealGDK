param(
    [string] $test_result_dir
)

if (Test-Path "$test_result_dir\index.html" -PathType Leaf) {
    # The Unreal Engine produces a mostly undocumented index.html/index.json as the result of running a test suite, for now seems mostly
    # for internal use - but it's an okay visualisation for test results, so we fix it up here to display as a build artifact in CI
    # (replacing local dependencies in the html by CDNs or correcting paths)

    $replacement_strings = @()
    $replacement_strings += @('/bower_components/font-awesome/css/font-awesome.min.css', 'https://cdnjs.cloudflare.com/ajax/libs/font-awesome/4.7.0/css/font-awesome.min.css')
    $replacement_strings += @('/bower_components/twentytwenty/css/twentytwenty.css', 'https://cdnjs.cloudflare.com/ajax/libs/mhayes-twentytwenty/1.0.0/css/twentytwenty.min.css')
    $replacement_strings += @('/bower_components/featherlight/release/featherlight.min.css', 'https://cdnjs.cloudflare.com/ajax/libs/featherlight/1.7.13/featherlight.min.css')
    $replacement_strings += @('/bower_components/bootstrap/dist/css/bootstrap.min.css', 'https://cdnjs.cloudflare.com/ajax/libs/twitter-bootstrap/3.3.7/css/bootstrap.min.css')
    $replacement_strings += @('/bower_components/jquery/dist/jquery.min.js', 'https://cdnjs.cloudflare.com/ajax/libs/jquery/3.1.1/jquery.min.js')
    $replacement_strings += @('/bower_components/jquery.event.move/js/jquery.event.move.js', 'https://cdnjs.cloudflare.com/ajax/libs/mhayes-twentytwenty/1.0.0/js/jquery.event.move.min.js')
    $replacement_strings += @('/bower_components/jquery_lazyload/jquery.lazyload.js', 'https://cdnjs.cloudflare.com/ajax/libs/jquery_lazyload/1.9.7/jquery.lazyload.min.js')
    $replacement_strings += @('/bower_components/twentytwenty/js/jquery.twentytwenty.js', 'https://cdnjs.cloudflare.com/ajax/libs/mhayes-twentytwenty/1.0.0/js/jquery.twentytwenty.min.js')
    $replacement_strings += @('/bower_components/clipboard/dist/clipboard.min.js', 'https://cdnjs.cloudflare.com/ajax/libs/clipboard.js/1.5.16/clipboard.min.js')
    $replacement_strings += @('/bower_components/anchor-js/anchor.min.js', 'https://cdnjs.cloudflare.com/ajax/libs/anchor-js/3.2.2/anchor.min.js')
    $replacement_strings += @('/bower_components/featherlight/release/featherlight.min.js', 'https://cdnjs.cloudflare.com/ajax/libs/featherlight/1.7.13/featherlight.min.js')
    $replacement_strings += @('/bower_components/bootstrap/dist/js/bootstrap.min.js', 'https://cdnjs.cloudflare.com/ajax/libs/twitter-bootstrap/3.3.7/js/bootstrap.min.js')
    $replacement_strings += @('/bower_components/dustjs-linkedin/dist/dust-full.min.js', 'https://cdnjs.cloudflare.com/ajax/libs/dustjs-linkedin/2.7.5/dust-full.min.js')
    $replacement_strings += @('/bower_components/numeral/min/numeral.min.js', 'https://cdnjs.cloudflare.com/ajax/libs/numeral.js/2.0.4/numeral.min.js')

    for ($i = 0; $i -lt $replacement_strings.length; $i = $i + 2) {
        $first = $replacement_strings[$i]
        $second = $replacement_strings[$i+1]
        ((Get-Content -Path "$test_result_dir\index.html" -Raw) -Replace $first, $second) | Set-Content -Path "$test_result_dir\index.html"
    }

    # %5C is the escape code for a backslash \, needed to successfully reach the artifact from the serving site
    ((Get-Content -Path "$test_result_dir\index.html" -Raw) -Replace "index.json", "ci%5CTestResults%5Cindex.json") | Set-Content -Path "$test_result_dir\index.html"

    echo "Test results in a nicer format can be found <a href='artifact://ci\TestResults\index.html'>here</a>.`n" | Out-File "$gdk_home/annotation.md"

    Get-Content "$gdk_home/annotation.md" | buildkite-agent annotate `
        --context "unreal-gdk-test-artifact-location"  `
        --style info
    
    Write-Log "Test results are displayed in a nicer form in the artifacts (index.html / index.json)"
}

## Read the test results
$results_path = Join-Path -Path $test_result_dir -ChildPath "index.json"
$results_json = Get-Content $results_path -Raw

$results_obj = ConvertFrom-Json $results_json

$tests_passed = $results_obj.failed -ne 0

if ($env:BUILDKITE_BRANCH -eq "master" -Or ((Test-Path env:BUILDKITE_SLACK_NOTIFY) -And $env:BUILDKITE_SLACK_NOTIFY -eq "true")) {
    # Send a Slack notification with a link to the build.
    # Read Slack webhook secret from the vault and extract the Slack webhook URL from it.
    $slack_webhook_secret = "$(imp-ci secrets read --environment=production --buildkite-org=improbable --secret-type=slack-webhook --secret-name=unreal-gdk-slack-web-hook)"
    $slack_webhook_url = $slack_webhook_secret | ConvertFrom-Json | %{$_.url}

    $gdk_commit_url = "https://github.com/spatialos/UnrealGDK/commit/$env:BUILDKITE_COMMIT"
    $build_url = "$env:BUILDKITE_BUILD_URL"
    
    $json_message = [ordered]@{
        text = $(if ((Test-Path env:BUILDKITE_NIGHTLY_BUILD) -And $env:BUILDKITE_NIGHTLY_BUILD -eq "true") {":night_with_stars: Nightly build of GDK for Unreal"} `
                else {"GDK for Unreal build by $env:BUILDKITE_BUILD_CREATOR"}) + $(if ($tests_passed) {" passed testing."} else {" failed testing."})
        attachments= @(
                @{
                    fallback = "Find the build at $build_url"
                    color = $(if ($tests_passed) {"good"} else {"bad"})
                    fields = @(
                            @{
                                title = "Build Message"
                                value = "$env:BUILDKITE_MESSAGE"
                                short = "true"
                            }
                            @{
                                title = "GDK branch"
                                value = "$env:BUILDKITE_BRANCH"
                                short = "true"
                            }
                            @{
                                title = "Tests passed"
                                value = "$($results_obj.succeeded) / $($results_obj.succeeded + $results_obj.failed)"
                                short = "true"
                            }
                        )
                    actions = @(
                            @{
                                type = "button"
                                text = ":github: View GDK commit"
                                url = "$gdk_commit_url"
                                style = "primary"
                            }
                            @{
                                type = "button"
                                text = ":buildkite: View build"
                                url = "$build_url"
                                style = "primary"
                            }
                            @{
                                type = "button"
                                text = ":buildkite: Test results"
                                url = "$build_url"
                                style = "primary"
                            }
                            @{
                                type = "button"
                                text = ":buildkite: Test log"
                                url = "$build_url"
                                style = "primary"
                            }
                        )
                }
            )
        }

    $json_request = $json_message | ConvertTo-Json -Depth 10

    Invoke-WebRequest -UseBasicParsing "$slack_webhook_url" -ContentType "application/json" -Method POST -Body "$json_request"
}

## Fail this build if any tests failed
if ($tests_passed) {
    $fail_msg = "$($results_obj.failed) tests failed. Logs for these tests are contained in the tests.log artifact."
    Write-Log $fail_msg
    Throw $fail_msg
}

Write-Log "All tests passed!"