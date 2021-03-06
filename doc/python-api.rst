.. Copyright (c) 2016-2020 The Regents of the University of Michigan
.. This file is part of the General Simulation Data (GSD) project, released
.. under the BSD 2-Clause License.

gsd python package
==================

**GSD** provides a **Python** API intended for most users. Developers, or users not working with the Python language,
may want to use the :ref:`c_api_`.

Submodules
----------

.. toctree::
   :maxdepth: 3

   python-module-gsd.fl
   python-module-gsd.hoomd
   python-module-gsd.pygsd

Package contents
----------------

.. automodule:: gsd
    :synopsis: GSD main module.
    :members:

Logging
-------

All python modules in **GSD** use the python standard library module :py:mod:`logging` to log events. Use this module
to control the verbosity and output destination::

    import logging
    logging.basicConfig(level=logging.INFO)

.. seealso::

    Module :py:mod:`logging`
        Documentation of the :py:mod:`logging` standard module.
