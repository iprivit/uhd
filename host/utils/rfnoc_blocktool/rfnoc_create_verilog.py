#!/usr/bin/env python
# -*- coding: utf-8 -*-
#
# Copyright 2013-2015 Ettus Research LLC
# Copyright 2018 Ettus Research, a National Instruments Brand
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
"""
Create a task for each file to generate from templates. A task is based on
a named tuple to ease future extensions.
Each task generates on result file by utilizing the BlockGenerator class.
"""

import argparse
import datetime
import os
import re
from collections import namedtuple
import six
import mako.template
import mako.lookup
import yaml

def dictionary2object(dictionary):
    """Transforms a dictionary into an object.

    This converts a dictionary to an object, where each key maps to a member in
    the object. If the value of the dictionary itself is a dictionary
    dictionary2object is called recursively to resolve all dictionaries in the
    hierarchy.


    :param dictionary: root dictionary
    :return:           object hierarchy
    """
    root = type('dictionary_wrapper', (object,), dictionary)
    seqs = tuple, list, set, frozenset
    for key, value in six.iteritems(dictionary):
        if isinstance(value, dict):
            setattr(root, key, dictionary2object(value))
        elif isinstance(value, seqs):
            setattr(root, key,
                    type(value)(dictionary2object(item)
                                if isinstance(item, dict)
                                else item for item in value))
        else:
            setattr(root, key, value)
    return root

class BlockGenerator:
    """
    Generates a new file from a template utilizing a YAML configuration passed
    as argument.

    Each BlockGenerator generate one file out of one template.
    All substitution parameter used in the template must be given in the YAML
    configuration. Exceptions: year (generated on the fly) and destination
    (given as argument). The root object parsed from the YAML configuration is
    config. All configuration items are represented as object members of config
    (YAML dictionaries are resolved recursively).
    """

    def __init__(self, template_file):
        """
        Initializes a new generator based on template_file
        :param template_file: file used as root template during rendering,
                            template file is assumed to be located in folder
                            'modules' relative to this Python file.
        """
        self.template_file = template_file
        self.year = datetime.datetime.now().year
        self.parser = None
        self.config = None
        self.destination = None

    def setup_parser(self):
        """
        Setup argument parser.

        ArgumentParser is able to receive destination path and a file location
        of the YAML configuration.
        """
        self.parser = argparse.ArgumentParser(
            description="Generate RFNoC module out of yml description")
        self.parser.add_argument("-c", "--config", required=True,
                                 help="Path to yml configuration file")
        self.parser.add_argument("-d", "--destination", required=True,
                                 help="Destination path to write output files")

    def setup(self):
        """
        Prepare generator for template substitution.

        Results of setup are save in member variables and are accessible for
        further template processing.
        self.year         current year (for copyright headers)
        self.destination  root path where template generation results are placed
        self.config       everything that was passed as YAML format
                          configuration, to ease access to configuration
                          parameters they are converted to an object hierarchy
                          such that template engine can use config.key1.key2
                          instead of config["key1"]["key2"]
        """
        args = self.parser.parse_args()
        self.year = datetime.datetime.now().year
        self.destination = args.destination
        os.makedirs(self.destination, exist_ok=True)
        with open(args.config) as stream:
            config = yaml.load(stream)
            self.config = dictionary2object(config)

    def run(self):
        """
        Do template substitution for destination template using YAML
        configuration passed by arguments.

        Template substitution is done in memory. The result is written to a file
        where the destination folder is given by the argument parser and the
        final filename is derived from the template file by substitute template
        by the module name from the YAML configuration.
        """
        lookup = mako.lookup.TemplateLookup(directories=['.'])
        filename = os.path.join("templates", self.template_file)
        tpl = mako.template.Template(filename=filename, lookup=lookup,
                                     strict_undefined=True)
        # Render and return
        block = tpl.render(**{"config": self.config,
                              "year": self.year,
                              "destination": self.destination,})

        filename = self.template_file
        # pylint: disable=no-member
        filename = re.sub(r"template(.*)\.mako",
                          r"%s\1" % self.config.module_name,
                          filename)
        fullpath = os.path.join(self.destination, filename)

        with open(fullpath, "w") as stream:
            stream.write(block)


if __name__ == "__main__":
    Task = namedtuple("Task", "name")
    TASKS = [Task(name="noc_shell_template.v.mako"),
             Task(name="rfnoc_block_template.v.mako"),
             Task(name="rfnoc_block_template_tb.sv.mako"),
             Task(name="Makefile"),
             Task(name="Makefile.srcs")]
    for task in TASKS:
        generator = BlockGenerator(task.name)
        generator.setup_parser()
        generator.setup()
        generator.run()
