#!/usr/bin/env python3

import os
import logging

import requests

from requests.adapters import HTTPAdapter  # type: ignore
from urllib3.util.retry import Retry  # type: ignore

from get_previous_release_tag import ReleaseInfo, get_previous_release

CLICKHOUSE_TAGS_URL = "https://api.github.com/repos/ClickHouse/ClickHouse/tags"

DOWNLOAD_PREFIX = (
    "https://github.com/ClickHouse/ClickHouse/releases/download/v{version}-{type}/"
)
CLICKHOUSE_COMMON_STATIC_PACKAGE_NAME = "clickhouse-common-static_{version}_amd64.deb"
CLICKHOUSE_COMMON_STATIC_DBG_PACKAGE_NAME = (
    "clickhouse-common-static-dbg_{version}_amd64.deb"
)
CLICKHOUSE_SERVER_PACKAGE_NAME = "clickhouse-server_{version}_amd64.deb"
CLICKHOUSE_SERVER_PACKAGE_FALLBACK = "clickhouse-server_{version}_all.deb"
CLICKHOUSE_CLIENT_PACKAGE_NAME = "clickhouse-client_{version}_amd64.deb"
CLICKHOUSE_CLIENT_PACKAGE_FALLBACK = "clickhouse-client_{version}_all.deb"

PACKAGES_DIR = "previous_release_package_folder/"
VERSION_PATTERN = r"((?:\d+\.)?(?:\d+\.)?(?:\d+\.)?\d+-[a-zA-Z]*)"


def download_package(url, out_path, retries=10, backoff_factor=0.3):
    session = requests.Session()
    retry = Retry(
        total=retries,
        read=retries,
        connect=retries,
        backoff_factor=backoff_factor,
        status_forcelist=[500, 502, 503, 504],
    )
    adapter = HTTPAdapter(max_retries=retry)
    session.mount("http://", adapter)
    session.mount("https://", adapter)
    response = session.get(url)
    response.raise_for_status()
    print(f"Download {url} to {out_path}")
    with open(out_path, "wb") as fd:
        fd.write(response.content)


def download_packages(release, dest_path=PACKAGES_DIR):
    if not os.path.exists(dest_path):
        os.makedirs(dest_path)

    logging.info("Will download %s", release)

    def get_dest_path(pkg_name):
        return os.path.join(dest_path, pkg_name)

    for pkg in (
        CLICKHOUSE_COMMON_STATIC_PACKAGE_NAME,
        CLICKHOUSE_COMMON_STATIC_DBG_PACKAGE_NAME,
    ):
        url = (DOWNLOAD_PREFIX + pkg).format(version=release.version, type=release.type)
        pkg_name = get_dest_path(pkg.format(version=release.version))
        download_package(url, pkg_name)

    for pkg, fallback in (
        (CLICKHOUSE_SERVER_PACKAGE_NAME, CLICKHOUSE_SERVER_PACKAGE_FALLBACK),
        (CLICKHOUSE_CLIENT_PACKAGE_NAME, CLICKHOUSE_CLIENT_PACKAGE_FALLBACK),
    ):
        url = (DOWNLOAD_PREFIX + pkg).format(version=release.version, type=release.type)
        pkg_name = get_dest_path(pkg.format(version=release.version))
        try:
            download_package(url, pkg_name)
        except Exception:
            url = (DOWNLOAD_PREFIX + fallback).format(
                version=release.version, type=release.type
            )
            pkg_name = get_dest_path(fallback.format(version=release.version))
            download_package(url, pkg_name)


def download_last_release(dest_path):
    current_release = get_previous_release(None)
    download_packages(current_release, dest_path=dest_path)


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO)
    release = ReleaseInfo(input())
    download_packages(release)
