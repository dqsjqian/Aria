"""Aria's dependency selection; build recipes live in ForgeRecipes."""
from pathlib import Path

import forgerecipes
from forgedeps import ProjectConfig
from forgerecipes import get_recipe
from forgerecipes.configuration import PATCH_VERSIONS
from forgerecipes.openssl import build_openssl


def make_config():
    return ProjectConfig(
        name="aria", recipes=tuple(get_recipe(name) for name in ("json", "doctest", "openssl")),
        patches_dir=Path(forgerecipes.__file__).with_name("patches"),
        patch_versions=PATCH_VERSIONS, builders={"openssl": build_openssl},
    )
