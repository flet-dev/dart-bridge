function publish_to_pypi() {
    if [[ ("$APPVEYOR_REPO_BRANCH" == "main" || "$APPVEYOR_REPO_TAG_NAME" != "") && "$APPVEYOR_PULL_REQUEST_NUMBER" == "" ]]; then
        twine upload "$@"
    elif [[ "$APPVEYOR_PULL_REQUEST_NUMBER" == "" ]]; then
        for wheel in "$@"; do
            curl -F package=@$wheel https://$GEMFURY_TOKEN@push.fury.io/flet/
        done
    fi
}